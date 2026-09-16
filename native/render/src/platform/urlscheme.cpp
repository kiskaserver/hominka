#include "platform/urlscheme.h"

#ifdef _WIN32
#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#include "app/runtime.h"

namespace hominka {

namespace {

// Номер повідомлення всередині WM_COPYDATA: щоб випадковий чужий WM_COPYDATA
// (а їх шлють і оболонка, і деякі утиліти) не потрапив нам у тему.
const ULONG_PTR kCopyDataMagic = 0x484F4D31;   // "HOM1"

const wchar_t* kWindowClass = L"HominkaRenderOverlay";

std::mutex g_mx;
std::string g_delivered;

std::wstring exe_path() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

// Те саме значення вже лежить? Тоді не чіпаємо.
//
// Не заради швидкості: запис у реєстр — це подія, яку бачать і антивіруси, і
// системи поведінкового аналізу. Робити її на кожному старті, щоб записати те
// саме, — дарма додавати собі підозрілості. А оновлення шлях міняє, тож
// перевіряти таки треба щоразу.
bool same_value(HKEY root, const wchar_t* path, const wchar_t* name,
                const std::wstring& want) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    wchar_t buf[1024];
    DWORD size = sizeof buf, type = 0;
    const LSTATUS rc = RegQueryValueExW(key, name, nullptr, &type, (BYTE*)buf, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return false;
    const size_t chars = size / sizeof(wchar_t);
    std::wstring have(buf, chars ? chars - 1 : 0);   // без кінцевого нуля
    return have == want;
}

bool set_key(HKEY root, const wchar_t* path, const wchar_t* name, const std::wstring& value) {
    if (same_value(root, path, name, value)) return true;
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const LSTATUS rc = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                                      (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

}  // namespace

void register_url_scheme() {
    const std::wstring exe = exe_path();
    if (exe.empty()) return;

    // Той самий запис, що робить будь-яка програма зі своїм посиланням.
    // HKCU, а не HKLM: прав адміністратора в нас немає й не треба, а діє це
    // для того, хто програму й запускає.
    const std::wstring open = L"\"" + exe + L"\" \"%1\"";
    bool ok = set_key(HKEY_CURRENT_USER, L"Software\\Classes\\hominka", nullptr,
                      L"URL:Hominka");
    ok = set_key(HKEY_CURRENT_USER, L"Software\\Classes\\hominka", L"URL Protocol", L"") && ok;
    ok = set_key(HKEY_CURRENT_USER, L"Software\\Classes\\hominka\\DefaultIcon", nullptr,
                 L"\"" + exe + L"\",0") && ok;
    ok = set_key(HKEY_CURRENT_USER, L"Software\\Classes\\hominka\\shell\\open\\command",
                 nullptr, open) && ok;
    if (!ok) rlog("посилання hominka:// зареєструвати не вийшло");
}

std::string theme_id_from_url(const std::string& url) {
    const char* kPrefix = "hominka://theme/";
    const size_t n = strlen(kPrefix);
    if (url.size() <= n || _strnicmp(url.c_str(), kPrefix, (int)n) != 0) return "";

    std::string id = url.substr(n);
    // Хвіст після імені (слеш, запит, якір) відрізаємо — нам потрібне саме ім'я.
    const size_t cut = id.find_first_of("/?#");
    if (cut != std::string::npos) id.erase(cut);
    while (!id.empty() && (id.back() == '\r' || id.back() == '\n' || id.back() == ' '))
        id.pop_back();

    if (id.empty() || id.size() > 32) return "";
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return "";
    }
    return id;
}

bool forward_to_running(const std::string& url) {
    HWND h = FindWindowW(kWindowClass, nullptr);
    if (!h) return false;

    COPYDATASTRUCT cd = {};
    cd.dwData = kCopyDataMagic;
    cd.cbData = (DWORD)(url.size() + 1);
    cd.lpData = (void*)url.c_str();
    // SendMessageTimeout, а не SendMessage: та копія може бути зайнята
    // малюванням, і висіти тут, поки вона звільниться, нам ні до чого.
    DWORD_PTR res = 0;
    const LRESULT sent = SendMessageTimeoutW(h, WM_COPYDATA, 0, (LPARAM)&cd,
                                             SMTO_ABORTIFHUNG, 3000, &res);
    return sent != 0 && res != 0;
}

void deliver_url(const std::string& url) {
    std::lock_guard<std::mutex> lock(g_mx);
    g_delivered = url;
}

bool handle_copydata(unsigned msg, void* lparam) {
    if (msg != WM_COPYDATA || !lparam) return false;
    const COPYDATASTRUCT* cd = (const COPYDATASTRUCT*)lparam;
    if (cd->dwData != kCopyDataMagic || !cd->lpData || cd->cbData == 0) return false;

    std::string url((const char*)cd->lpData, cd->cbData);
    while (!url.empty() && url.back() == '\0') url.pop_back();
    deliver_url(url);
    rlog("прийшло посилання: %s", url.c_str());
    return true;
}

std::string take_delivered_url() {
    std::lock_guard<std::mutex> lock(g_mx);
    std::string out;
    out.swap(g_delivered);
    return out;
}

}  // namespace hominka

#else   // не Windows

namespace hominka {

// Під Linux програма живе в AppImage, і своє посилання там реєструє не вона, а
// .desktop у системі. Доки цього немає — чесна заглушка: сайт покаже кнопку
// «Скопіювати», яка працює скрізь.
void register_url_scheme() {}
std::string theme_id_from_url(const std::string&) { return ""; }
void deliver_url(const std::string&) {}
bool forward_to_running(const std::string&) { return false; }
std::string take_delivered_url() { return ""; }
bool handle_copydata(unsigned, void*) { return false; }

}  // namespace hominka

#endif
