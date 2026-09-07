#include "ipc.h"

#ifdef _WIN32

#include <cstdio>
#include <cstring>

namespace hominka {

namespace {
// Читаємо великими шматками: сплеск чату — це десятки повідомлень і кілька
// картинок по 10–50 КБ, і дробити їх на дрібні читання немає сенсу.
const DWORD kChunk = 64 * 1024;
const DWORD kBufferSize = 512 * 1024;

uint32_t read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
}  // namespace

bool IpcServer::start(uint32_t owner_pid, const char* suffix) {
    // Ім'я збираємо у вузьких символах, а потім розширюємо: суфікс приходить
    // з переносимої підписи, і тримати його двома видами ні до чого.
    char narrow[160];
    _snprintf(narrow, 160, "%s%lu%s", "\\\\.\\pipe\\Hominka-",
              (unsigned long)owner_pid, suffix ? suffix : "");
    name_ = narrow;
    wchar_t buf[160];
    _snwprintf(buf, 160, L"%hs", narrow);
    wname_ = buf;
    chunk_.resize(kChunk);
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bevent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event_ || !bevent_) return false;
    if (!create_pipe()) return false;
    // Другий канал не критичний: без нього рендер просто не зможе розповісти
    // про дії людини, але чат малюватиме як і раніше.
    create_back_pipe();
    return true;
}

bool IpcServer::create_pipe() {
    // PIPE_TYPE_BYTE: межі кадрів визначає наша власна довжина, а не канал —
    // так само працює і з сокетом, якщо колись знадобиться.
    pipe_ = CreateNamedPipeW(wname_.c_str(),
                             PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                             1, kBufferSize, kBufferSize, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    begin_connect();
    return true;
}

bool IpcServer::create_back_pipe() {
    const std::wstring back_name = wname_ + L"-back";
    back_ = CreateNamedPipeW(back_name.c_str(),
                             PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                             1, kBufferSize, kBufferSize, 0, nullptr);
    if (back_ == INVALID_HANDLE_VALUE) return false;
    begin_back_connect();
    return true;
}

void IpcServer::begin_back_connect() {
    memset(&bov_, 0, sizeof(bov_));
    bov_.hEvent = bevent_;
    ResetEvent(bevent_);
    back_connected_ = false;
    if (ConnectNamedPipe(back_, &bov_)) { back_pending_ = false; back_connected_ = true; return; }
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) { back_pending_ = false; back_connected_ = true; }
    else if (err == ERROR_IO_PENDING) { back_pending_ = true; }
    else { back_pending_ = false; }
}

void IpcServer::poll_back() {
    if (back_ == INVALID_HANDLE_VALUE || !back_pending_) return;
    DWORD n = 0;
    if (GetOverlappedResult(back_, &bov_, &n, FALSE)) {
        back_pending_ = false;
        back_connected_ = true;
    } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
        DisconnectNamedPipe(back_);
        begin_back_connect();
    }
}

void IpcServer::begin_connect() {
    memset(&ov_, 0, sizeof(ov_));
    ov_.hEvent = event_;
    ResetEvent(event_);
    connected_ = false;
    reading_ = false;
    if (ConnectNamedPipe(pipe_, &ov_)) {
        pending_connect_ = false;
        connected_ = true;
        return;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
        // Клієнт устиг підключитися між створенням і викликом — це не помилка.
        pending_connect_ = false;
        connected_ = true;
        SetEvent(event_);
    } else if (err == ERROR_IO_PENDING) {
        pending_connect_ = true;
    } else {
        pending_connect_ = false;
    }
}

bool IpcServer::take(size_t n, uint8_t* dst) {
    if (buf_.size() < n) return false;
    if (dst) memcpy(dst, buf_.data(), n);
    buf_.erase(buf_.begin(), buf_.begin() + n);
    return true;
}

void IpcServer::poll(std::vector<IpcFrame>* out) {
    poll_back();
    if (pipe_ == INVALID_HANDLE_VALUE) return;

    // 1) Чекаємо на підключення (не блокуючись).
    if (pending_connect_) {
        DWORD n = 0;
        if (GetOverlappedResult(pipe_, &ov_, &n, FALSE)) {
            pending_connect_ = false;
            connected_ = true;
        } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
            // Щось пішло не так — канал перестворюємо, інакше залишимося глухі.
            DisconnectNamedPipe(pipe_);
            begin_connect();
            return;
        } else {
            return;
        }
    }
    if (!connected_) return;

    // 2) Забираємо все, що прийшло.
    for (;;) {
        if (!reading_) {
            memset(&ov_, 0, sizeof(ov_));
            ov_.hEvent = event_;
            ResetEvent(event_);
            DWORD got = 0;
            if (ReadFile(pipe_, chunk_.data(), kChunk, &got, &ov_)) {
                if (got) buf_.insert(buf_.end(), chunk_.begin(), chunk_.begin() + got);
                continue;                      // могло лишитися ще
            }
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) { reading_ = true; break; }
            // Клієнт відвалився — чекаємо на наступного (Hominka могла
            // перезапустити свій бік, а нам через це падати не варто).
            DisconnectNamedPipe(pipe_);
            begin_connect();
            return;
        }

        DWORD got = 0;
        if (!GetOverlappedResult(pipe_, &ov_, &got, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) break;
            reading_ = false;
            DisconnectNamedPipe(pipe_);
            begin_connect();
            return;
        }
        reading_ = false;
        if (got) buf_.insert(buf_.end(), chunk_.begin(), chunk_.begin() + got);
    }

    // 3) Ріжемо накопичене на кадри. Неповний кадр лишається в буфері до
    //    наступного разу — саме тому межі й задані довжинами, а не роздільником.
    for (;;) {
        if (buf_.size() < 4) return;
        const uint32_t head_len = read_u32(buf_.data());
        if (head_len > kBufferSize) {           // сміття в потоці — рвемо звʼязок
            buf_.clear();
            DisconnectNamedPipe(pipe_);
            begin_connect();
            return;
        }
        if (buf_.size() < 4 + head_len + 4) return;
        const uint32_t blob_len = read_u32(buf_.data() + 4 + head_len);
        const size_t total = 4 + head_len + 4 + blob_len;
        if (buf_.size() < total) return;

        IpcFrame f;
        f.json.assign((const char*)buf_.data() + 4, head_len);
        if (blob_len) {
            const uint8_t* p = buf_.data() + 4 + head_len + 4;
            f.blob.assign(p, p + blob_len);
        }
        take(total, nullptr);
        out->push_back(std::move(f));
    }
}

bool IpcServer::send(const std::string& json, const uint8_t* blob, size_t blob_len) {
    if (back_ == INVALID_HANDLE_VALUE || !back_connected_) return false;

    // Той самий формат, що й у другий бік: довжина заголовка, заголовок,
    // довжина вкладення, вкладення.
    std::vector<uint8_t> frame;
    frame.reserve(json.size() + 8 + blob_len);
    const uint32_t n = (uint32_t)json.size();
    for (int i = 0; i < 4; ++i) frame.push_back((uint8_t)((n >> (i * 8)) & 0xFF));
    frame.insert(frame.end(), json.begin(), json.end());
    const uint32_t bn = (uint32_t)blob_len;
    for (int i = 0; i < 4; ++i) frame.push_back((uint8_t)((bn >> (i * 8)) & 0xFF));
    if (blob && blob_len) frame.insert(frame.end(), blob, blob + blob_len);

    OVERLAPPED wov = {};
    wov.hEvent = bevent_;
    ResetEvent(bevent_);
    DWORD written = 0;
    if (WriteFile(back_, frame.data(), (DWORD)frame.size(), &written, &wov))
        return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    // Чекаємо коротко: повідомлення зазвичай крихітні, а стати надовго рендер
    // не має права — він у цей час малює. Кадр предпросмотру більший, тож і
    // терпіння трохи більше.
    const DWORD wait_ms = blob_len ? 500 : 50;
    if (WaitForSingleObject(bevent_, wait_ms) != WAIT_OBJECT_0) {
        CancelIoEx(back_, &wov);
        return false;
    }
    return GetOverlappedResult(back_, &wov, &written, FALSE) != 0;
}

void IpcServer::stop() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(pipe_);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (back_ != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(back_);
        CloseHandle(back_);
        back_ = INVALID_HANDLE_VALUE;
    }
    if (event_) { CloseHandle(event_); event_ = nullptr; }
    if (bevent_) { CloseHandle(bevent_); bevent_ = nullptr; }
    connected_ = false;
    back_connected_ = false;
}

}  // namespace hominka

#endif  // _WIN32
