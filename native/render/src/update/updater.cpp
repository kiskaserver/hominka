#include "update/updater.h"

#ifdef _WIN32

#include <windows.h>
#include <shlwapi.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

#include <mbedtls/sha256.h>

#include "net/net_http.h"

namespace hominka {

namespace {

const char* kUpdateBase = "https://update.svitix.com/hominka/";
const wchar_t* kUserAgent = L"Hominka-Updater";

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out((size_t)(n > 0 ? n - 1 : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::string temp_dir() {
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    return n ? narrow(std::wstring(buf, n)) : std::string(".\\");
}

std::string system32(const char* tool) {
    wchar_t dir[MAX_PATH] = {0};
    if (!GetSystemDirectoryW(dir, MAX_PATH)) return tool;
    return narrow(dir) + "\\" + tool;
}

// Тека, у якій лежить сама програма. Саме її й підмінюємо.
std::string app_dir() {
    wchar_t path[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return "";
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? "" : narrow(s.substr(0, slash));
}

std::string hex(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += kHex[data[i] >> 4];
        out += kHex[data[i] & 0xF];
    }
    return out;
}

// Завантаження архіва — на WinHTTP, а не на тій самій бібліотеці, що й решта
// запитів.
//
// Причина одна й вагома: архів важить понад двісті мегабайтів, а IXWebSocket
// віддає тіло відповіді цілим рядком у пам'яті. Для програми, чий сенс — не
// займати пам'ять, це було б смішно. WinHTTP віддає потік, тож ми пишемо
// одразу у файл і рахуємо sha256 на льоту.
bool download_to_file(const std::string& url, const std::string& out_path,
                      long long expect_size, std::string* sha_out, std::string* error,
                      std::atomic<int>* percent) {
    URL_COMPONENTS uc = {sizeof(uc)};
    wchar_t host[256] = {0}, path[2048] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    const std::wstring wurl = widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        *error = "не розібрав посилання";
        return false;
    }

    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { *error = "не піднявся HTTP"; return false; }

    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr,
                                              WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS
                                                  ? WINHTTP_FLAG_SECURE
                                                  : 0)
                         : nullptr;
    FILE* out = nullptr;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);

    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            *error = "сервер відповів " + std::to_string((int)status);
        } else if (!(out = fopen(out_path.c_str(), "wb"))) {
            *error = "нема куди писати архів";
        } else {
            mbedtls_sha256_starts(&sha, 0);
            std::vector<unsigned char> buf(256 * 1024);
            long long done = 0;
            ok = true;
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) {
                    *error = "з'єднання обірвалося";
                    ok = false;
                    break;
                }
                if (!got) break;
                if (fwrite(buf.data(), 1, got, out) != got) {
                    *error = "не записалося на диск";
                    ok = false;
                    break;
                }
                mbedtls_sha256_update(&sha, buf.data(), got);
                done += got;
                if (expect_size > 0 && percent)
                    *percent = (int)(done * 100 / expect_size);
            }
            if (ok) {
                unsigned char digest[32] = {0};
                mbedtls_sha256_finish(&sha, digest);
                *sha_out = hex(digest, sizeof digest);
            }
        }
    } else if (error->empty()) {
        *error = "не вдалося звернутися до сервера оновлень";
    }

    if (out) fclose(out);
    mbedtls_sha256_free(&sha);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    if (!ok) DeleteFileA(out_path.c_str());
    return ok;
}

// Підмінник. Запущену програму не можна перезаписати самою собою (у Windows
// файл узагалі замкнений), тому копіює нове й перезапускає нас окремий процес —
// він і чекає нашого виходу.
//
// Літерал СИРИЙ (R"(...)"), і це не смак: у Python-версії цей самий текст
// одного разу записали звичайним рядком, «\tasklist» стало табуляцією, і
// підмінник тихо перестав і чекати, і копіювати. Виглядало це найгірше з
// можливого: програма чесно закривалася, оновлення не ставилося, і сказати про
// це було нікому.
//
// Усі команди — повними шляхами з System32: інакше все залежить від PATH, де
// раніше може трапитися чужий find.exe з іншою поведінкою.
const char* kUpdateBat = R"(@echo off
setlocal enabledelayedexpansion
set SYS=%SystemRoot%\System32\
set LOG=%TEMP%\hominka-update.log
echo [%DATE% %TIME%] wait pid=%1 src=%2 dst=%3 >> "%LOG%"

:wait
%SYS%tasklist.exe /NH /FI "PID eq %1" 2>nul | %SYS%find.exe "%1" >nul
if not errorlevel 1 (
  %SYS%ping.exe -n 2 127.0.0.1 >nul
  goto wait
)

REM Навіть після виходу процесу файл ще секунду-дві буває замкнений. Пробуємо
REM перейменувати його — це найдешевша перевірка «чи можна писати».
set TRIES=0
:trylock
set /a TRIES+=1
%SYS%ping.exe -n 2 127.0.0.1 >nul
ren "%~3\Hominka.exe" "Hominka.exe.old" 2>nul
if errorlevel 1 (
  if !TRIES! LSS 15 goto trylock
  echo [%DATE% %TIME%] exe still locked, copying anyway >> "%LOG%"
) else (
  ren "%~3\Hominka.exe.old" "Hominka.exe" 2>nul
)

echo [%DATE% %TIME%] copying >> "%LOG%"
%SYS%robocopy.exe "%~2" "%~3" /E /IS /IT /R:5 /W:2 /NFL /NDL /NJH /NJS >> "%LOG%" 2>&1
echo [%DATE% %TIME%] robocopy exit=%ERRORLEVEL% >> "%LOG%"

for %%A in ("%~2\Hominka.exe") do set SRCSIZE=%%~zA
for %%A in ("%~3\Hominka.exe") do set DSTSIZE=%%~zA
echo [%DATE% %TIME%] size src=!SRCSIZE! dst=!DSTSIZE! >> "%LOG%"
if not "!SRCSIZE!"=="!DSTSIZE!" (
  echo [%DATE% %TIME%] size mismatch, retry once >> "%LOG%"
  %SYS%ping.exe -n 4 127.0.0.1 >nul
  %SYS%robocopy.exe "%~2" "%~3" /E /IS /IT /R:5 /W:2 /NFL /NDL /NJH /NJS >> "%LOG%" 2>&1
)

echo [%DATE% %TIME%] restarting >> "%LOG%"
start "" "%~3\Hominka.exe"
rmdir /s /q "%~4"
(goto) 2>nul & del "%~f0"
)";

// Слід того, що текст скрипта колись переписали не сирим рядком. Краще
// зупинитися тут і сказати вголос, поки вікно ще на екрані, ніж запустити
// підмінника, який нічого не зробить.
bool script_sane(const char* text) {
    for (const char* p = text; *p; ++p)
        if (*p == '\t' || *p == '\r' || *p == '\f' || *p == '\v' || *p == '\b') return false;
    return true;
}

}  // namespace

Updater::~Updater() {
    if (worker_.joinable()) worker_.join();
}

Release Updater::release() const {
    std::lock_guard<std::mutex> lock(mx_);
    return release_;
}

std::string Updater::error() const {
    std::lock_guard<std::mutex> lock(mx_);
    return error_;
}

void Updater::check(const std::string& channel, const std::string& current,
                    const std::string& installed_channel) {
    if (state_ == State::Checking || state_ == State::Downloading) return;
    if (worker_.joinable()) worker_.join();
    state_ = State::Checking;
    worker_ = std::thread(&Updater::run_check, this, channel, current, installed_channel);
}

void Updater::run_check(std::string channel, std::string current, std::string installed) {
    const HttpResult r = http_get(std::string(kUpdateBase) + channel + ".json", 20);
    if (!r.ok()) {
        std::lock_guard<std::mutex> lock(mx_);
        error_ = r.status ? ("сервер оновлень відповів " + std::to_string(r.status))
                          : ("сервер оновлень недоступний: " + r.error);
        state_ = State::Failed;
        return;
    }

    std::string err;
    const Release rel = parse_manifest(r.body, channel, &err);
    if (!err.empty()) {
        std::lock_guard<std::mutex> lock(mx_);
        error_ = err;
        state_ = State::Failed;
        return;
    }

    // Оновлення потрібне, якщо версія новіша АБО людина щойно перемкнула канал.
    const bool switched = !installed.empty() && installed != channel;
    const bool want = is_newer(rel.version, current) ||
                      (switched && rel.version != current);
    std::lock_guard<std::mutex> lock(mx_);
    release_ = rel;
    error_.clear();
    state_ = want ? State::Available : State::UpToDate;
}

void Updater::download() {
    if (state_ != State::Available) return;
    if (worker_.joinable()) worker_.join();
    percent_ = 0;
    state_ = State::Downloading;
    worker_ = std::thread(&Updater::run_download, this, release());
}

void Updater::run_download(Release rel) {
    char name[64];
    snprintf(name, sizeof name, "hominka-%s.zip", rel.version.c_str());
    const std::string path = temp_dir() + name;

    std::string got, err;
    if (!download_to_file(rel.url, path, rel.size, &got, &err, &percent_)) {
        std::lock_guard<std::mutex> lock(mx_);
        error_ = err;
        state_ = State::Failed;
        return;
    }
    // Обірване завантаження або підміна — розпаковувати не можна.
    if (!rel.sha256.empty() && got != rel.sha256) {
        DeleteFileA(path.c_str());
        std::lock_guard<std::mutex> lock(mx_);
        error_ = "контрольна сума не збіглася — архів пошкоджено або підмінено";
        state_ = State::Failed;
        return;
    }
    std::lock_guard<std::mutex> lock(mx_);
    zip_path_ = path;
    error_.clear();
    percent_ = 100;
    state_ = State::Ready;
}

std::string Updater::install() {
    std::string zip;
    {
        std::lock_guard<std::mutex> lock(mx_);
        zip = zip_path_;
    }
    if (state_ != State::Ready || zip.empty()) return "нема чого встановлювати";

    const std::string dir = app_dir();
    if (dir.empty()) return "не знайшов теку програми";

    // Розпаковуємо ПОРУЧ із програмою, а не в тимчасову теку: копіювати потім
    // доведеться в межах того самого диска, і це швидко.
    const size_t slash = dir.find_last_of("\\/");
    const std::string staging =
        (slash == std::string::npos ? dir : dir.substr(0, slash)) + "\\Hominka_update";

    // Розпаковування — системним tar.exe (він є в кожній Windows 10 і новіших і
    // розуміє zip). Вкладати заради цього ще одну бібліотеку ні до чого, а
    // архів на цей момент уже перевірено підписом і сумою.
    const std::string tar = system32("tar.exe");
    if (GetFileAttributesA(tar.c_str()) == INVALID_FILE_ATTRIBUTES)
        return "у системі немає tar.exe — оновіться вручну з сайту";

    RemoveDirectoryA(staging.c_str());
    CreateDirectoryA(staging.c_str(), nullptr);

    std::string cmd = "\"" + tar + "\" -x -f \"" + zip + "\" -C \"" + staging + "\"";
    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return "не вдалося розпакувати архів";
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (rc != 0) return "архів не розпакувався";

    DeleteFileA(zip.c_str());
    cleanup_downloads();

    // Архів може бути запакований як «Hominka/…» — тоді працюємо з підтекою.
    std::string src = staging + "\\Hominka";
    if (GetFileAttributesA((src + "\\Hominka.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
        src = staging;
    if (GetFileAttributesA((src + "\\Hominka.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
        return "в архіві немає Hominka.exe";

    if (!script_sane(kUpdateBat)) return "скрипт оновлення зіпсований — оновіться вручну";
    const std::string bat = temp_dir() + "hominka-update.bat";
    {
        // cp1251: скрипт читає консоль зі своєю кодовою сторінкою, а латиниці
        // й розділових знаків у ньому досить — кирилиці в командах немає.
        FILE* f = fopen(bat.c_str(), "wb");
        if (!f) return "нема куди записати скрипт оновлення";
        fwrite(kUpdateBat, 1, strlen(kUpdateBat), f);
        fclose(f);
    }

    char pid[32];
    snprintf(pid, sizeof pid, "%lu", (unsigned long)GetCurrentProcessId());
    std::string run = "\"" + system32("cmd.exe") + "\" /c \"" + bat + "\" " + pid +
                      " \"" + src + "\" \"" + dir + "\" \"" + staging + "\"";
    STARTUPINFOA si2 = {sizeof(si2)};
    PROCESS_INFORMATION pi2 = {};
    // Підмінник переживає наш вихід і сам по собі; вікна консолі посеред гри
    // людині не потрібно.
    if (!CreateProcessA(nullptr, &run[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr,
                        &si2, &pi2))
        return "не вдалося запустити підмінника";
    CloseHandle(pi2.hThread);
    CloseHandle(pi2.hProcess);
    return "";
}

int Updater::cleanup_downloads(const std::string& keep) {
    const std::string dir = temp_dir();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "hominka-*.zip").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int removed = 0;
    do {
        const std::string path = dir + fd.cFileName;
        if (!keep.empty() && path == keep) continue;
        if (DeleteFileA(path.c_str())) ++removed;   // зайнятий — приберемо наступного разу
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return removed;
}

}  // namespace hominka

#endif  // _WIN32
