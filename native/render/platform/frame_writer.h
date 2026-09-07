// Кадр чату у спільну памʼять — для оверлея, вкладеного в гру.
//
// Навіщо це тут, коли чат уже видно поверх гри через DirectComposition. Це два
// РІЗНІ шляхи, і другий нікуди не дівається:
//   * DirectComposition — окреме вікно поверх гри, у гру нічого не вкладається.
//     Працює всюди, безпечно для античитів, але в грі з ексклюзивним
//     повноекранним його не буде.
//   * інжект (native/overlay/) — overlay.dll усередині процесу гри домальовує
//     чат прямо в її кадр. Працює й в ексклюзивному, але це вкладання в чужий
//     процес, тож лише для одиночних ігор і з жорсткою відмовою в іграх з
//     античитами (common/guard.h).
// Домовленість про пікселі в них одна: common/shared_frame.h.
//
// Що змінилося. Раніше цей кадр робив Python: знімав картинку з вікна браузера
// (grab), рахував CRC по всьому кадру, щоб зрозуміти, чи він змінився, і
// перекладав байти в спільну памʼять. Тепер кадр уже намальовано — лишається
// забрати його з відеокарти й покласти. CRC не потрібен: ми й так знаємо, чи
// малювали цього разу.
//
// Читання з відеокарти коштує грошей, тому робимо його ЛИШЕ коли інжект
// увімкнено. Просто «чат поверх гри» через DComp цим шляхом не ходить взагалі.
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "common/shared_frame.h"

namespace hominka {

class FrameWriter {
public:
    ~FrameWriter() { close(); }

    // Відкриває (або створює) спільну памʼять. Другий продюсер не потрібен:
    // іменований м'ютекс тримає той, хто перший, — так само, як це робив
    // Python. Не вдалося взяти — значить, кадр уже пише хтось інший.
    bool open() {
        if (view_) return true;
        mutex_ = CreateMutexW(nullptr, TRUE, L"Local\\HominkaOverlayProducer");
        if (!mutex_) return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
            conflict_ = true;
            return false;
        }
        conflict_ = false;

        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  SHARED_FRAME_TOTAL, HOMINKA_SHARED_FRAME_NAME);
        if (!map_) { close(); return false; }
        view_ = (uint8_t*)MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_FRAME_TOTAL);
        if (!view_) { close(); return false; }
        return true;
    }

    bool conflict() const { return conflict_; }
    bool ready() const { return view_ != nullptr; }

    // Кладе кадр під seqlock: непарний seq → пікселі → парний seq. Той, хто
    // читає, бачить непарне або зміну seq і просто пропускає кадр, малюючи
    // попередній. Ні мʼютексів, ні очікувань — гра не стоїть через нас ніколи.
    //
    // pos/size — рамка чату в частках кадру гри [0..1]: де й якого розміру
    // вікно чату на СВОЄМУ моніторі, там і такого ж розміру чат у грі.
    void write(const uint8_t* bgra, int w, int h,
               float pos_x, float pos_y, float size_x, float size_y,
               uint32_t opacity, uint32_t target_pid, bool hide_from_obs) {
        if (!view_ || w <= 0 || h <= 0) return;
        if ((uint32_t)w > SHARED_FRAME_MAX_W || (uint32_t)h > SHARED_FRAME_MAX_H) return;
        const uint32_t stride = (uint32_t)w * 4;
        if (stride * (uint32_t)h > SHARED_FRAME_DATA_MAX) return;

        SharedFrameHeader hdr = {};
        hdr.magic = SHARED_FRAME_MAGIC;
        hdr.version = SHARED_FRAME_VERSION;
        hdr.width = (uint32_t)w;
        hdr.height = (uint32_t)h;
        hdr.stride = stride;
        hdr.pos_x = pos_x;
        hdr.pos_y = pos_y;
        hdr.size_x = size_x;
        hdr.size_y = size_y;
        hdr.opacity = opacity > 255 ? 255 : opacity;
        hdr.enabled = 1;
        hdr.heartbeat = ++heartbeat_;
        hdr.target_pid = target_pid;
        hdr.hide_from_obs = hide_from_obs ? 1 : 0;

        seq_ |= 1u;                       // непарне: «пишу»
        hdr.seq = seq_;
        memcpy(view_, &hdr, sizeof(hdr));
        memcpy(view_ + SHARED_FRAME_HEADER_SIZE, bgra, (size_t)stride * h);

        seq_ = (seq_ + 1u) & 0xFFFFFFFFu; // парне: «готово»
        hdr.seq = seq_;
        memcpy(view_, &hdr, sizeof(hdr));
    }

    // «Чату немає» — DLL перестає малювати. Пікселі не чіпаємо: вони й не
    // потрібні, поки enabled = 0.
    void disable() {
        if (!view_) return;
        SharedFrameHeader hdr = {};
        memcpy(&hdr, view_, sizeof(hdr));
        if (hdr.magic != SHARED_FRAME_MAGIC) return;
        seq_ |= 1u;
        hdr.seq = seq_;
        hdr.enabled = 0;
        memcpy(view_, &hdr, sizeof(hdr));
        seq_ = (seq_ + 1u) & 0xFFFFFFFFu;
        hdr.seq = seq_;
        memcpy(view_, &hdr, sizeof(hdr));
    }

    void close() {
        if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
        if (map_) { CloseHandle(map_); map_ = nullptr; }
        // Звільняє позначку «продюсер активний».
        if (mutex_) { CloseHandle(mutex_); mutex_ = nullptr; }
    }

private:
    HANDLE map_ = nullptr;
    HANDLE mutex_ = nullptr;
    uint8_t* view_ = nullptr;
    uint32_t seq_ = 0;
    uint32_t heartbeat_ = 0;
    bool conflict_ = false;
};

}  // namespace hominka
