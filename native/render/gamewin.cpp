#include "gamewin.h"

#ifdef _WIN32

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace hominka {

namespace {

// Токен запуску інжектора — дзеркало HOMINKA_INJECT_TOKEN з
// native/common/secret.h. Без нього інжектор відмовляє: щоб ним не
// користувалися окремо від програми.
const char* kInjectToken = "HMK-INJ-7F3A9C21-64bd-4e0a-choose-your-game";

// Коди виходу інжектора (native/injector/injector.cpp).
const DWORD EX_OK = 0, EX_NO_PROC = 2, EX_BLOCKED = 3, EX_BITNESS = 4, EX_INJECT = 5;

// Прапорець «без повноекранної оптимізації» в гілці сумісності.
const char* kLayersKey = "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
const char* kFsoFlag = "DISABLEDXMAXIMIZEDWINDOWEDMODE";

// DLL графічних API, які вміє малювати наш оверлей. Якщо в процесі немає
// ЖОДНОЇ — це не гра (нотатник, провідник, консоль…), і вкладатися туди нема
// сенсу.
const char* kGraphicsDlls[] = {"d3d9.dll",     "d3d11.dll",    "d3d12.dll",
                               "dxgi.dll",     "opengl32.dll", "vulkan-1.dll"};

// Наші власні вікна й системна обслуга — не ігри.
const char* kOurs[] = {"hominka.exe", "hominka-render-x64.exe", "python.exe", "pythonw.exe"};
const char* kSkip[] = {"explorer.exe", "applicationframehost.exe", "textinputhost.exe",
                       "systemsettings.exe", "searchhost.exe", "shellexperiencehost.exe",
                       "startmenuexperiencehost.exe"};

// Що ми зробили безрамковим — щоб було чим повернути як було.
struct Saved {
    LONG style;
    LONG exstyle;
    RECT rect;
};
std::map<HWND, Saved> g_changed;

std::string narrow(const std::wstring& s) {
    if (s.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

unsigned pid_of(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return (unsigned)pid;
}

// Повний шлях до .exe процесу. Потрібен і для назви в списку, і для запису
// прапорця сумісності — той ключується саме шляхом.
std::wstring exe_path_of(unsigned pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH] = {0};
    DWORD len = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &len);
    CloseHandle(h);
    return ok ? std::wstring(buf, len) : L"";
}

std::string exe_name_of(unsigned pid) {
    const std::wstring path = exe_path_of(pid);
    const size_t slash = path.find_last_of(L"\\/");
    return narrow(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

bool in_list(const std::string& name, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i) if (name == list[i]) return true;
    return false;
}

// Прапорці сумісності для .exe як окремі слова. «~» — обов'язковий маркер шару
// на початку; без нього прапорці ігноруються.
std::vector<std::string> layers_tokens(const std::string& exe_path) {
    std::vector<std::string> out;
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kLayersKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return out;
    char buf[1024] = {0};
    DWORD len = sizeof buf, type = 0;
    if (RegQueryValueExA(key, exe_path.c_str(), nullptr, &type, (BYTE*)buf, &len) ==
            ERROR_SUCCESS &&
        type == REG_SZ) {
        std::string all(buf);
        size_t pos = 0;
        while (pos < all.size()) {
            while (pos < all.size() && all[pos] == ' ') ++pos;
            const size_t end = all.find(' ', pos);
            const std::string tok = all.substr(pos, end == std::string::npos ? end : end - pos);
            if (!tok.empty()) out.push_back(tok);
            if (end == std::string::npos) break;
            pos = end + 1;
        }
    }
    RegCloseKey(key);
    return out;
}

std::set<std::string> loaded_modules(unsigned pid, bool* known) {
    std::set<std::string> out;
    *known = false;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return out;      // гра від адміністратора — не знаємо, і це не «порожньо»

    HMODULE mods[1024];
    DWORD needed = 0;
    if (EnumProcessModulesEx(h, mods, sizeof mods, &needed, LIST_MODULES_ALL)) {
        const size_t count = needed / sizeof(HMODULE);
        wchar_t name[MAX_PATH];
        for (size_t i = 0; i < count && i < 1024; ++i)
            if (GetModuleBaseNameW(h, mods[i], name, MAX_PATH))
                out.insert(lower(narrow(name)));
        *known = true;
    }
    CloseHandle(h);
    return out;
}

// Чи схоже, що в процесі гра з підтримуваним графічним API. Не змогли
// перелічити модулі — вважаємо, що так: краще спробувати, ніж дарма відмовити
// грі, запущеній від адміністратора.
bool has_graphics_api(unsigned pid) {
    bool known = false;
    const std::set<std::string> mods = loaded_modules(pid, &known);
    if (!known) return true;
    for (const char* dll : kGraphicsDlls) if (mods.count(dll)) return true;
    return false;
}

bool target_is_64(unsigned pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return true;     // не дізналися — хай вирішує сам інжектор
    BOOL wow = FALSE;
    const bool ok = IsWow64Process(h, &wow) != 0;
    CloseHandle(h);
    return ok ? !wow : true;
}

}  // namespace

// Де лежать нативні частини: поруч із нами або в dist під час розробки.
std::string native_dir() {
    wchar_t path[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return "";
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return "";
    const std::string here = narrow(dir.substr(0, slash));

    // Беремо перший каталог, де маркер СПРАВДІ лежить, а не просто той, що
    // існує: інакше в dev ми б спинилися на порожньому native/.
    const std::string cands[] = {here, here + "\\native", here + "\\native\\dist"};
    for (const std::string& c : cands)
        if (GetFileAttributesA((c + "\\injector-x64.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
            return c;
    return here;
}

namespace {

RECT monitor_rect(HWND hwnd) {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

struct EnumCtx {
    std::vector<GameWindow>* out;
    std::set<std::string>* seen;
};

BOOL CALLBACK enum_cb(HWND hwnd, LPARAM lparam) {
    EnumCtx* ctx = (EnumCtx*)lparam;
    if (!IsWindowVisible(hwnd)) return TRUE;
    // Дочірні й власні спливні вікна не показуємо — потрібне головне вікно гри.
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    std::wstring title((size_t)len + 1, L'\0');
    GetWindowTextW(hwnd, &title[0], len + 1);
    title.resize((size_t)len);
    if (title.find_first_not_of(L" \t") == std::wstring::npos) return TRUE;

    const unsigned pid = pid_of(hwnd);
    const std::string exe = exe_name_of(pid);
    const std::string low = lower(exe);
    if (low.empty()) return TRUE;
    if (in_list(low, kOurs, sizeof kOurs / sizeof *kOurs)) return TRUE;
    if (in_list(low, kSkip, sizeof kSkip / sizeof *kSkip)) return TRUE;

    // Одна гра — один рядок, навіть якщо вікон у неї кілька.
    const std::string key = std::to_string(pid) + ":" + low;
    if (!ctx->seen->insert(key).second) return TRUE;

    GameWindow w;
    w.hwnd = hwnd;
    w.pid = pid;
    w.exe = exe;
    w.title = narrow(title);
    ctx->out->push_back(w);
    return TRUE;
}

}  // namespace

std::vector<GameWindow> list_windows() {
    std::vector<GameWindow> out;
    std::set<std::string> seen;
    EnumCtx ctx = {&out, &seen};
    EnumWindows(enum_cb, (LPARAM)&ctx);
    std::sort(out.begin(), out.end(), [](const GameWindow& a, const GameWindow& b) {
        return lower(a.title) < lower(b.title);
    });
    return out;
}

bool make_borderless(void* h) {
    HWND hwnd = (HWND)h;
    if (!hwnd || g_changed.count(hwnd)) return false;

    Saved s;
    s.style = GetWindowLongW(hwnd, GWL_STYLE);
    s.exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    GetWindowRect(hwnd, &s.rect);
    g_changed[hwnd] = s;

    LONG style = s.style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                             WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
    style |= WS_POPUP;
    const LONG ex = s.exstyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
                                  WS_EX_WINDOWEDGE);
    SetWindowLongW(hwnd, GWL_STYLE, style);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);

    const RECT m = monitor_rect(hwnd);
    SetWindowPos(hwnd, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

bool restore_window(void* h) {
    HWND hwnd = (HWND)h;
    auto it = g_changed.find(hwnd);
    if (it == g_changed.end()) return false;
    const Saved s = it->second;
    g_changed.erase(it);

    SetWindowLongW(hwnd, GWL_STYLE, s.style);
    SetWindowLongW(hwnd, GWL_EXSTYLE, s.exstyle);
    SetWindowPos(hwnd, nullptr, s.rect.left, s.rect.top, s.rect.right - s.rect.left,
                 s.rect.bottom - s.rect.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void* borderless_window() {
    return g_changed.empty() ? nullptr : (void*)g_changed.begin()->first;
}

std::string game_exe_path(void* hwnd) {
    return narrow(exe_path_of(pid_of((HWND)hwnd)));
}

bool fso_disabled(const std::string& exe_path) {
    if (exe_path.empty()) return false;
    for (const std::string& t : layers_tokens(exe_path)) if (t == kFsoFlag) return true;
    return false;
}

bool set_fso_disabled(const std::string& exe_path, bool disabled) {
    if (exe_path.empty()) return false;

    // Чужі прапорці сумісності зберігаємо: людина могла виставити їх сама, і
    // затерти їх заразом означало б поламати те, чого ми не ставили.
    std::vector<std::string> tokens;
    for (const std::string& t : layers_tokens(exe_path))
        if (t != "~" && t != kFsoFlag) tokens.push_back(t);

    std::string value;
    if (disabled) {
        value = std::string("~ ") + kFsoFlag;
        for (const std::string& t : tokens) value += " " + t;
    } else if (!tokens.empty()) {
        value = "~";
        for (const std::string& t : tokens) value += " " + t;
    }

    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kLayersKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok;
    if (value.empty()) {
        // Нічого не лишилось — прибираємо запис зовсім.
        RegDeleteValueA(key, exe_path.c_str());
        ok = true;
    } else {
        ok = RegSetValueExA(key, exe_path.c_str(), 0, REG_SZ, (const BYTE*)value.c_str(),
                            (DWORD)value.size() + 1) == ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return ok;
}

bool injector_available() {
    const std::string d = native_dir();
    return GetFileAttributesA((d + "\\injector-x64.exe").c_str()) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesA((d + "\\overlay-x64.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}

InjectResult inject_into(void* h) {
    InjectResult res;
    if (!injector_available()) {
        res.message = "Нативні файли не знайдено — перевстановіть програму.";
        return res;
    }
    const unsigned pid = pid_of((HWND)h);
    if (!pid) {
        res.message = "Не вдалося визначити гру.";
        return res;
    }

    // Вкладаємося ЛИШЕ в ігри з підтримуваним графічним API. У звичайну
    // програму без нього оверлею нема куди малювати — тож чесно відмовляємо ще
    // до вкладення.
    if (!has_graphics_api(pid)) {
        res.blocked = true;
        res.message = "У цьому вікні немає гри з підтримуваним графічним API "
                      "(DirectX 9/11/12, OpenGL чи Vulkan) — оверлей працює лише в іграх.";
        return res;
    }

    const std::string dir = native_dir();
    const char* order[2] = {"x64", "x86"};
    if (!target_is_64(pid)) { order[0] = "x86"; order[1] = "x64"; }

    res.message = "Не вдалося показати чат у грі.";
    for (const char* arch : order) {
        const std::string exe = dir + "\\injector-" + arch + ".exe";
        if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

        // Шлях до DLL НЕ передаємо: інжектор бере лише власну overlay й звіряє
        // в ній маркер.
        std::string cmd = "\"" + exe + "\" --pid " + std::to_string(pid) + " --token " +
                          kInjectToken;
        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi)) {
            res.message = "Інжектор не запустився.";
            continue;
        }
        WaitForSingleObject(pi.hProcess, 20000);
        DWORD code = EX_INJECT;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (code == EX_OK) {
            res.ok = true;
            res.pid = pid;
            res.message = "Чат у грі увімкнено.";
            return res;
        }
        if (code == EX_BLOCKED) {
            res.blocked = true;
            res.message = "Ця гра із захистом від сторонніх програм (античит) — вкладати "
                          "щось у неї не можна, це загрожує баном. Лишається безрамковий "
                          "режим.";
            return res;
        }
        if (code == EX_BITNESS) continue;          // не та розрядність — пробуємо інший
        if (code == EX_NO_PROC) {
            res.message = "Гра закрилася, поки ми до неї йшли.";
            continue;
        }
        res.message = "Система не пустила в процес гри. Якщо гра запущена від імені "
                      "адміністратора, запустіть так само і Hominka.";
    }
    return res;
}

}  // namespace hominka

#endif  // _WIN32
