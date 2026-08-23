// hominka-inject — кладе overlay.dll у процес гри.
//
//   hominka-inject.exe --pid 1234 --dll C:\...\overlay64.dll
//   hominka-inject.exe --exe game.exe --dll C:\...\overlay64.dll
//   hominka-inject.exe --pid 1234 --check          (тільки перевірити, не чіпати)
//
// Спосіб класичний і той самий, яким користуються Steam, Discord і OBS:
// виділити памʼять у чужому процесі, покласти туди шлях до бібліотеки і
// попросити його ж LoadLibraryW цю бібліотеку відкрити. Нічого не
// приховуємо: ні від системи, ні від антивіруса, ні від самої гри.
//
// Коди виходу — щоб програма могла сказати людині щось конкретніше за
// «не вдалося»:
//   0  готово
//   1  помилка в аргументах
//   2  ВІДМОВА: змагальна гра або античит (див. common/guard.h)
//   3  розрядність не збігається — потрібен інший інжектор
//   4  процес не знайдено
//   5  не пускає система (права)
//   6  сама інʼєкція не вдалася

#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdio.h>

#include "../common/guard.h"
#include "../common/log.h"

namespace {

void say(const wchar_t* fmt, ...) {
    wchar_t line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(line, 1023, fmt, ap);
    va_end(ap);
    line[1023] = 0;
    fwprintf(stderr, L"%ls\n", line);
    char utf8[1024];
    WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    hominka::log("inject: %s", utf8);
}

DWORD pid_by_name(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe) == 0) { found = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool name_of_pid(DWORD pid, wchar_t* out, size_t len) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool ok = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                wcsncpy(out, pe.szExeFile, len - 1);
                out[len - 1] = 0;
                ok = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return ok;
}

// 32- чи 64-розрядний процес. Покласти 64-бітну бібліотеку в 32-бітну гру
// неможливо — і навпаки; це не «не спрацювало», це різні світи.
bool is_wow64(HANDLE proc, bool* ok) {
    *ok = false;
    typedef BOOL (WINAPI *Wow64Process2)(HANDLE, USHORT*, USHORT*);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    Wow64Process2 fn = k32 ? (Wow64Process2)GetProcAddress(k32, "IsWow64Process2") : NULL;
    if (fn) {
        USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN, native = 0;
        if (fn(proc, &process_machine, &native)) {
            *ok = true;
            // UNKNOWN означає «не під WOW64», тобто процес рідної розрядності.
            return process_machine != IMAGE_FILE_MACHINE_UNKNOWN;
        }
    }
    BOOL wow = FALSE;
    if (IsWow64Process(proc, &wow)) { *ok = true; return wow != FALSE; }
    return false;
}

const wchar_t* arg_after(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], key) == 0) return argv[i + 1];
    return NULL;
}

bool has_flag(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], key) == 0) return true;
    return false;
}

}  // namespace

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    const wchar_t* pid_s = arg_after(argc, argv, L"--pid");
    const wchar_t* exe_s = arg_after(argc, argv, L"--exe");
    const wchar_t* dll_s = arg_after(argc, argv, L"--dll");
    const bool check_only = has_flag(argc, argv, L"--check");

    if ((!pid_s && !exe_s) || (!dll_s && !check_only)) {
        say(L"Використання: hominka-inject --pid <pid>|--exe <name.exe> "
            L"--dll <шлях> [--check]");
        return 1;
    }

    DWORD pid = pid_s ? (DWORD)_wtoi(pid_s) : pid_by_name(exe_s);
    if (!pid) {
        say(L"Процес не знайдено: %ls", exe_s ? exe_s : pid_s);
        return 4;
    }

    wchar_t name[MAX_PATH] = L"";
    if (!name_of_pid(pid, name, MAX_PATH)) {
        say(L"Процес %lu зник, поки ми до нього йшли", pid);
        return 4;
    }

    // Спершу відмова, і лише потім усе інше: жодного відкриття процесу, поки
    // не вирішено, що туди взагалі можна.
    wchar_t matched[MAX_PATH] = L"";
    hominka::GuardVerdict verdict = hominka::check_process(pid, name, matched, MAX_PATH);
    if (verdict == hominka::GUARD_BLOCKED_GAME) {
        say(L"ВІДМОВА: %ls — змагальна гра з античитом. Стороння бібліотека в її "
            L"процесі — привід для бану акаунта, тож ми туди не полізаємо. "
            L"Для таких ігор лишається безрамковий режим.", matched);
        return 2;
    }
    if (verdict == hominka::GUARD_BLOCKED_ANTICHEAT) {
        say(L"ВІДМОВА: у процесі %ls працює античит (%ls). Не наша територія.",
            name, matched);
        return 2;
    }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        DWORD e = GetLastError();
        say(L"Не пускає до процесу %ls (%lu): помилка %lu. "
            L"Гра, запущена від імені адміністратора, вимагає того самого від нас.",
            name, pid, e);
        return 5;
    }

    bool bitness_known = false;
    bool target_wow = is_wow64(proc, &bitness_known);
    const bool we_are_64 = sizeof(void*) == 8;
    // 64-розрядна система: WOW64 = 32-бітний процес. Нам треба збігу.
    const bool target_is_64 = !target_wow;
    if (bitness_known && target_is_64 != we_are_64) {
        say(L"Розрядність не збігається: %ls — %ls, а цей інжектор — %ls. "
            L"Запустіть hominka-inject%ls.exe",
            name, target_is_64 ? L"64-бітний" : L"32-бітний",
            we_are_64 ? L"64-бітний" : L"32-бітний",
            target_is_64 ? L"64" : L"32");
        CloseHandle(proc);
        return 3;
    }

    if (check_only) {
        say(L"OK: %ls (pid %lu, %ls) — можна", name, pid,
            target_is_64 ? L"64-біт" : L"32-біт");
        CloseHandle(proc);
        return 0;
    }

    wchar_t dll_full[MAX_PATH];
    if (!GetFullPathNameW(dll_s, MAX_PATH, dll_full, NULL) ||
        GetFileAttributesW(dll_full) == INVALID_FILE_ATTRIBUTES) {
        say(L"Немає файлу бібліотеки: %ls", dll_s);
        CloseHandle(proc);
        return 1;
    }

    const SIZE_T bytes = (wcslen(dll_full) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        say(L"Не вдалося виділити памʼять у процесі: помилка %lu", GetLastError());
        CloseHandle(proc);
        return 6;
    }
    if (!WriteProcessMemory(proc, remote, dll_full, bytes, NULL)) {
        say(L"Не вдалося записати шлях у процес: помилка %lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 6;
    }

    // kernel32.dll лежить за однією адресою в усіх процесах однієї розрядності
    // (ASLR розігрується раз на завантаження системи), тому адресу LoadLibraryW
    // можна взяти в себе й передати туди.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC load = k32 ? GetProcAddress(k32, "LoadLibraryW") : NULL;
    if (!load) {
        say(L"Не знайшли LoadLibraryW — цього не мало стат