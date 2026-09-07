// Канал між Hominka (Python) і рендером чату.
//
// Домовленість навмисно проста, бо по ній ходить дуже небагато: повідомлення
// чату, стилі, розкладка й байти картинок. Кадр виглядає так:
//
//     [4 байти little-endian] довжина заголовка
//     [заголовок]             JSON у UTF-8
//     [4 байти little-endian] довжина вкладення (0, якщо його немає)
//     [вкладення]             сирі байти (картинка)
//
// Чому не «просто рядки з переносами»: у кадр треба класти двійкові дані
// (PNG/WebP емоута), а рядкам вони чужі. Base64 роздув би трафік на третину і
// коштував би зайвого розбору на кожен емоут.
//
// Хто кого чекає. Сервер — ЦЕЙ процес: він створює канал одразу, ще до того як
// намалює перший кадр, і живе далі незалежно від того, підключився клієнт чи
// ні. Впала Hominka — рендер просто перестає отримувати повідомлення й тихо
// виходить услід за нею (за PID батька), не лишаючи оверлея-сироту на екрані.
//
// Типи заголовків (поле "t"):
//   msg      — повідомлення чату; решта полів — те, що будує chatsources.message()
//   delete   — прибрати повідомлення {"id": …}
//   purge    — прибрати всі повідомлення автора {"nick": …}
//   clear    — очистити стрічку
//   css      — свій CSS користувача {"css": …}
//   layout   — порядок частин рядка {"layout": [...]}
//   config   — вигляд: {"zoom":…, "opacity":…, "width":…, "height":…}
//   image    — картинка: {"url": …} + вкладення з байтами
//   enabled  — показувати/сховати {"on": true|false}
//   inject   — кадр для оверлея, ВКЛАДЕНОГО в гру:
//              {"on":…, "pid":…, "opacity":…, "hide_obs":…}. Поки on=false,
//              кадр із відеокарти не читається взагалі
//   bye      — Hominka просить завершитися
//   shot     — знімок поточного кадру в PNG {"path": …} (для перевірок)
//
// Каналів ДВА, і це не примха. Спершу був один двосторонній, але Python на
// своєму боці відкриває його одним файловим дескриптором, а рантайм C на
// Windows бере на дескриптор замок: потік-читач стоїть у ReadFile, тримає
// замок, і потік-писар не може написати нічого. Тому:
//     \.\pipe\Hominka-<pid>        — команди сюди (Hominka → рендер)
//     \.\pipe\Hominka-<pid>-back   — події звідси (рендер → Hominka)
// Два дескриптори — два замки, і ніхто нікого не тримає.
//
// Поза Windows це сокети AF_UNIX із тими самими іменами по суті:
//     $XDG_RUNTIME_DIR/hominka-<pid>.sock       — команди сюди
//     $XDG_RUNTIME_DIR/hominka-<pid>-back.sock  — події звідси
// Замка на дескриптор там немає, але каналів однаково два: домовленість має
// бути одна на обидві системи, інакше Python довелося б писати двічі.
//
// У ЗВОРОТНИЙ бік (рендер → Hominka) ходить те, що людина зробила у вікні:
//   look     — покрутили прозорість/кегль {"opacity":…, "bg_alpha":…, "zoom":…}
//   lock     — замкнули/розімкнули {"on": true|false}
//   geometry — перетягнули або розтягнули {"x":…, "y":…, "w":…, "h":…}
//   settings — натиснули шестерню (Hominka має показати панель)
// Запиту «дай картинку» тут навмисно немає: Hominka качає емоути й значки
// наперед і шле сама, тож рендеру нема чого просити.
//   frame    — кадр предпросмотру {"w":…, "h":…} + вкладення з пікселями BGRA
//              (premultiplied). Лише в режимі --preview.
//
// Правда про налаштування лишається в Python: рендер не пише config.json сам,
// він лише каже, що сталося.
#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace hominka {

// Один прочитаний кадр.
struct IpcFrame {
    std::string json;                 // заголовок
    std::vector<uint8_t> blob;        // вкладення (може бути порожнім)
};

// Сервер іменованого каналу. Один клієнт за раз — більше й не треба: у чату
// один хазяїн.
class IpcServer {
public:
    ~IpcServer() { stop(); }

    // Ім'я містить PID, щоб два запущені екземпляри Hominka не билися за один
    // канал. Local-простір імен: і рендер, і Hominka — один сеанс одного
    // користувача, Global\ вимагав би привілею й світив би чат іншим сеансам.
    // suffix розрізняє канали, коли процесів рендера кілька. Порожній —
    // основний оверлей; «-preview» — предпросмотр у редакторі CSS, який живе
    // окремим процесом і не має нічого спільного з вікном на екрані.
    bool start(uint32_t owner_pid, const char* suffix = "");

    // Забирає всі кадри, що вже прийшли. Не блокує: рендер не має права стати
    // через те, що Hominka забарилася.
    void poll(std::vector<IpcFrame>* out);

    // Чи підключений клієнт зараз.
    bool connected() const { return connected_; }

    // Надсилає кадр назад Hominka. Вкладення буває одне — пікселі
    // предпросмотру; решта повідомлень у цей бік короткі й без нього.
    bool send(const std::string& json, const uint8_t* blob = nullptr, size_t blob_len = 0);

    void stop();

    // Ім'я каналу — для журналу. Рядок UTF-8 на обох системах: у журнал
    // однаково пишеться байтами.
    const std::string& name() const { return name_; }

private:
    // Дочитує рівно n байтів з накопиченого буфера; false — ще не всі прийшли.
    bool take(size_t n, uint8_t* dst);

    bool connected_ = false;
    std::string name_;
    std::vector<uint8_t> buf_;        // усе, що прийшло й ще не розібрано
    std::vector<uint8_t> chunk_;      // приймач одного читання

#ifdef _WIN32
    bool create_pipe();
    void begin_connect();
    bool create_back_pipe();
    void begin_back_connect();
    void poll_back();

    // Канал команд (Hominka → сюди).
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    OVERLAPPED ov_{};
    HANDLE event_ = nullptr;
    // Канал подій (звідси → Hominka).
    HANDLE back_ = INVALID_HANDLE_VALUE;
    OVERLAPPED bov_{};
    HANDLE bevent_ = nullptr;
    bool back_connected_ = false;
    bool back_pending_ = false;
    bool pending_connect_ = false;
    bool reading_ = false;
    std::wstring wname_;              // те саме ім'я, як його бачить Windows
#else
    // Сокети AF_UNIX. Слухавка окремо від з'єднання: клієнт може відпасти й
    // прийти знову, а канал має лишитися.
    int listen_fd_ = -1;
    int conn_fd_ = -1;
    int back_listen_fd_ = -1;
    int back_fd_ = -1;
    std::string path_;
    std::string back_path_;
    // Те, що не влізло в сокет за один раз. Запис неблокуючий: рендер не має
    // права стати через те, що Hominka не читає.
    std::vector<uint8_t> out_;
    void accept_pending();
    void flush_out();
#endif

};

}  // namespace hominka
