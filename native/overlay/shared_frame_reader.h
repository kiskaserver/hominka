// Читання кадру чату зі спільної памʼяті (див. common/shared_frame.h).
//
// Відкриваємо мапінг, який створив Python. Його може ще не бути (програму
// запускають після гри) — тоді просто чекаємо й пробуємо знову; гру це не
// стосується. Кадр забираємо через seqlock: якщо seq непарний або змінився під
// час копіювання, значить трапили на момент запису — цей кадр пропускаємо,
// намалюється попередній.
#pragma once

#include <windows.h>
#include <string.h>
#include <stdlib.h>

#include "../common/shared_frame.h"
#include "../common/log.h"

namespace hominka {

struct FrameView {
    bool valid = false;      // чи вдалося зняти узгоджений кадр
    bool enabled = false;    // чат увімкнено на боці продюсера
    uint32_t seq = 0;        // номер кадру — щоб не заливати текстуру двічі
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t anchor = 0;
    int32_t margin_x = 0;
    int32_t margin_y = 0;
    uint32_t opacity = 255;
    uint32_t target_pid = 0;          // 0 = будь-який процес
    uint32_t hide_from_obs = 0;       // 1 = малювати якнайглибше, ховаючись від OBS
    const uint8_t* pixels = nullptr;  // BGRA, дійсний доти, доки живий Reader
};

class SharedFrameReader {
public:
    ~SharedFrameReader() { close(); }

    // Пробує під'єднатися до мапінгу. Повертає false, якщо його ще немає —
    // це не помилка, а «Python ще не запустив чат».
    bool ensure_open() {
        if (view_) return true;
        HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, HOMINKA_SHARED_FRAME_NAME);
        if (!h) return false;
        void* p = MapViewOfFile(h, FILE_MAP_READ, 0, 0, SHARED_FRAME_TOTAL);
        if (!p) {
            CloseHandle(h);
            return false;
        }
        map_ = h;
        view_ = p;
        log("overlay: під'єднано до спільної памʼяті чату");
        return true;
    }

    // Знімає узгоджений кадр у out. Пікселі копіюємо у власний буфер, щоб
    // подальша заливка в текстуру не читала память, яку продюсер саме переписує.
    bool read(FrameView* out) {
        if (!view_) return false;
        const SharedFrameHeader* h = reinterpret_cast<const SharedFrameHeader*>(view_);

        if (h->magic != SHARED_FRAME_MAGIC || h->version != SHARED_FRAME_VERSION)
            return false;

        uint32_t seq1 = h->seq;
        if (seq1 & 1u) return false;              // запис саме триває

        uint32_t w = h->width, ht = h->height, stride = h->stride;
        uint32_t enabled = h->enabled, anchor = h->anchor, opacity = h->opacity;
        uint32_t target_pid = h->target_pid;
        uint32_t hide_obs = h->hide_from_obs;
        int32_t mx = h->margin_x, my = h->margin_y;
        if (w == 0 || ht == 0 || w > SHARED_FRAME_MAX_W || ht > SHARED_FRAME_MAX_H ||
            stride != w * 4)
            return false;

        size_t bytes = (size_t)stride * ht;
        if (bytes > buf_size_) {
            uint8_t* nb = (uint8_t*)realloc(buf_, bytes);
            if (!nb) return false;
            buf_ = nb;
            buf_size_ = bytes;
        }
        const uint8_t* src = reinterpret_cast<const uint8_t*>(view_) + SHARED_FRAME_HEADER_SIZE;
        memcpy(buf_, src, bytes);

        uint32_t seq2 = h->seq;
        if (seq2 != seq1) return false;           // кадр змінився під час копіювання

        out->valid = true;
        out->enabled = enabled != 0;
        out->seq = seq1;
        out->width = w;
        out->height = ht;
        out->anchor = anchor;
        out->margin_x = mx;
        out->margin_y = my;
        out->opacity = opacity > 255 ? 255 : opacity;
        out->target_pid = target_pid;
        out->hide_from_obs = hide_obs;
        out->pixels = buf_;
        return true;
    }

    void close() {
        if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
        if (map_) { CloseHandle(map_); map_ = nullptr; }
        if (buf_) { free(buf_); buf_ = nullptr; buf_size_ = 0; }
    }

private:
    HANDLE map_ = nullptr;
    void* view_ = nullptr;
    uint8_t* buf_ = nullptr;
    size_t buf_size_ = 0;
};

}  // namespace hominka
