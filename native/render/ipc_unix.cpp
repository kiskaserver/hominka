// Канал між Hominka і рендером — реалізація на сокетах AF_UNIX.
//
// Домовленість про кадри та сама, що й під Windows (див. ipc.h): міняється
// лише те, ЧИМ носяться байти. Python на цьому боці відкриває звичайний
// socket, тож його код відрізняється парою рядків, а не логікою.
//
// Чому неблокуючі сокети й ручний буфер виходу, а не потік-писар: рендер не
// має права стати. Якщо Hominka забарилася й не читає, ми складаємо
// невідправлене в out_ і йдемо малювати далі — кадр чату важливіший за
// доставленість події «покрутили повзунок».
#include "ipc.h"

#ifndef _WIN32

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hominka {

namespace {

const size_t kChunk = 64 * 1024;

uint32_t read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// Куди класти сокет. XDG_RUNTIME_DIR — тека саме цього користувача й саме
// цього сеансу; це найближче до Local\ у Windows. Немає її — /tmp, але тоді
// ім'я включає uid, щоб два користувачі не побилися за один шлях.
std::string sock_dir() {
    const char* run = getenv("XDG_RUNTIME_DIR");
    if (run && *run) return run;
    return "/tmp";
}

// Слухавка на заданому шляху. Неблокуюча: приймаємо клієнта в poll().
int make_listener(const std::string& path) {
    // Лишок від попереднього запуску (нас убили — файл лишився) заважає bind.
    unlink(path.c_str());

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) { close(fd); return -1; }
    memcpy(addr.sun_path, path.c_str(), path.size());

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    // Права: тільки власник. Чат — це приватне листування, і сокет у /tmp не
    // має бути відкритий усій машині.
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
    if (listen(fd, 1) != 0) { close(fd); unlink(path.c_str()); return -1; }
    return fd;
}

}  // namespace

bool IpcServer::start(uint32_t owner_pid, const char* suffix) {
    const std::string dir = sock_dir();
    char buf[256];
    snprintf(buf, sizeof buf, "%s/hominka-%u%s.sock", dir.c_str(),
             (unsigned)owner_pid, suffix ? suffix : "");
    path_ = buf;
    snprintf(buf, sizeof buf, "%s/hominka-%u%s-back.sock", dir.c_str(),
             (unsigned)owner_pid, suffix ? suffix : "");
    back_path_ = buf;
    name_ = path_;

    chunk_.resize(kChunk);
    listen_fd_ = make_listener(path_);
    if (listen_fd_ < 0) return false;
    // Другий канал не критичний: без нього рендер просто не зможе розповісти
    // про дії людини, але чат малюватиме як і раніше.
    back_listen_fd_ = make_listener(back_path_);
    return true;
}

void IpcServer::accept_pending() {
    if (listen_fd_ >= 0 && conn_fd_ < 0) {
        const int fd = accept4(listen_fd_, nullptr, nullptr,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) { conn_fd_ = fd; connected_ = true; }
    }
    if (back_listen_fd_ >= 0 && back_fd_ < 0) {
        const int fd = accept4(back_listen_fd_, nullptr, nullptr,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) back_fd_ = fd;
    }
}

bool IpcServer::take(size_t n, uint8_t* dst) {
    if (buf_.size() < n) return false;
    if (dst) memcpy(dst, buf_.data(), n);
    buf_.erase(buf_.begin(), buf_.begin() + n);
    return true;
}

void IpcServer::poll(std::vector<IpcFrame>* out) {
    accept_pending();
    flush_out();

    if (conn_fd_ >= 0) {
        for (;;) {
            const ssize_t got = recv(conn_fd_, chunk_.data(), chunk_.size(), 0);
            if (got > 0) {
                buf_.insert(buf_.end(), chunk_.data(), chunk_.data() + got);
                continue;
            }
            if (got == 0) {
                // Клієнт відпав. Слухавку лишаємо: Hominka може прийти знову.
                close(conn_fd_);
                conn_fd_ = -1;
                connected_ = false;
            }
            break;                     // EAGAIN або помилка — на цьому колі все
        }
    }

    // Розбираємо все, що вже назбиралося. Кадр цілий тоді, коли є обидві
    // довжини й обидва тіла; інакше чекаємо наступного кола.
    for (;;) {
        if (buf_.size() < 4) return;
        const uint32_t hlen = read_u32(buf_.data());
        if (buf_.size() < 4 + hlen + 4) return;
        const uint32_t blen = read_u32(buf_.data() + 4 + hlen);
        if (buf_.size() < 4 + hlen + 4 + blen) return;

        take(4, nullptr);
        IpcFrame fr;
        fr.json.resize(hlen);
        take(hlen, (uint8_t*)&fr.json[0]);
        take(4, nullptr);
        fr.blob.resize(blen);
        if (blen) take(blen, fr.blob.data());
        out->push_back(std::move(fr));
    }
}

void IpcServer::flush_out() {
    if (back_fd_ < 0 || out_.empty()) return;
    for (;;) {
        // ::send — саме глобальний: усередині класу це ім'я перекриває наш
        // власний метод send().
        const ssize_t sent = ::send(back_fd_, out_.data(), out_.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            out_.erase(out_.begin(), out_.begin() + sent);
            if (out_.empty()) return;
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        // З'єднання порвалося — накопичене викидаємо, інакше воно висітиме
        // вічно й з'їсть память.
        close(back_fd_);
        back_fd_ = -1;
        out_.clear();
        return;
    }
}

bool IpcServer::send(const std::string& json, const uint8_t* blob, size_t blob_len) {
    if (back_fd_ < 0) return false;

    std::vector<uint8_t> frame;
    frame.reserve(8 + json.size() + blob_len);
    auto put32 = [&frame](uint32_t v) {
        frame.push_back((uint8_t)(v & 0xFF));
        frame.push_back((uint8_t)((v >> 8) & 0xFF));
        frame.push_back((uint8_t)((v >> 16) & 0xFF));
        frame.push_back((uint8_t)((v >> 24) & 0xFF));
    };
    put32((uint32_t)json.size());
    frame.insert(frame.end(), json.begin(), json.end());
    put32((uint32_t)blob_len);
    if (blob && blob_len) frame.insert(frame.end(), blob, blob + blob_len);

    // Черга не безмежна: якщо Hominka не читає кілька секунд, кадри
    // предпросмотру набігають мегабайтами. Краще втратити подію, ніж память.
    const size_t kMaxOut = 8 * 1024 * 1024;
    if (out_.size() + frame.size() > kMaxOut) return false;

    out_.insert(out_.end(), frame.begin(), frame.end());
    flush_out();
    return true;
}

void IpcServer::stop() {
    if (conn_fd_ >= 0) { close(conn_fd_); conn_fd_ = -1; }
    if (back_fd_ >= 0) { close(back_fd_); back_fd_ = -1; }
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    if (back_listen_fd_ >= 0) { close(back_listen_fd_); back_listen_fd_ = -1; }
    // Прибираємо за собою: лишений файл сокета заважав би наступному запуску.
    if (!path_.empty()) { unlink(path_.c_str()); path_.clear(); }
    if (!back_path_.empty()) { unlink(back_path_.c_str()); back_path_.clear(); }
    connected_ = false;
    buf_.clear();
    out_.clear();
}

}  // namespace hominka

#endif  // !_WIN32
