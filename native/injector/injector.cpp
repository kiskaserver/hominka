// injector.exe — кладе overlay.dll у чужий процес.
//
// Класична схема без хитрощів: VirtualAllocEx під шлях до DLL,
// WriteProcessMemory туди, CreateRemoteThread на LoadLibraryW. Адреса
// LoadLibraryW у kernel32 однакова в усіх процесах тієї самої розрядності, тож
// передати її як точку входу потоку — законно.
//
// Хитрощів немає навмисно. Це інструмент для СВОЇХ ігор, і робить він рівно те,
// що написано, — жодного обходу захисту. Перед інʼєкцією питаємо guard.h: у
// змагальні ігри з античитом не лізе взагалі (див. native/common/guard.h).
//
// Розрядність важлива: у 32-бітний процес можна вкласти лише 32-бітну DLL, у
// 64-бітний — 64-бітну. Тому інжектор збирається у двох розрядностях, а яку
// саме DLL брати, вирішує за розрядністю ЦІЛІ (IsWow64Process2).
//
// Аргументи:  injector.exe <pid | --exe game.exe> [--dll шлях\overlay.dll]
// Коди виходу: 0 успіх, 2 не знайдено процес, 3 заборонено (античит),
//              4 не та розрядність DLL, 5 інша помилка інʼєкції.

#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string>

#include "../common/log.h"
#include "../common/guard.h"

using hominka::log;

namespace {

enum ExitCode { EX_OK = 0, EX_ARGS = 1, EX_NO_PROC = 2, EX_BLOCKED = 3,
                EX_BITNESS = 4, EX_INJECT = 5 };

// Розрядність процесу: true — 64-бітний. IsWow64Process2 чесно відповідає й на
// ARM, і на майбутніх машинах, чого стара IsWow64Process не вміла.
bool is_64bit(HANDLE proc, bool* ok) {
    *ok = false;
    USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    typedef BOOL (WINAPI *Fn)(HANDLE, USHORT*, USHORT*);
    Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    if (!fn) return false;
    if (!fn(proc, &process_machine, &native_machine)) return false;
    *ok = true;
    // process_machine == UNKNOWN означає «рідна розрядність системи».
    if (process_machine == IMAGE_FILE_MACHINE_UNKNOWN)
        return native_machine == IMAGE_FILE_MACHINE_AMD64 ||
               native_machine == IMAGE_FILE_MACHINE_ARM64;
    return process_machine == IMAGE_FILE_MACHINE_AMD64;
}

// Наша власна розрядність, відома на етапі компіляції.
bool self_is_64bit() {
#if defined(_WIN64)
    return true;
#else
    return false;
#endif
}

DWORD find_pid_by_exe(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

bool exe_name_of(DWORD pid, wchar_t* out, size_t out_len) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                wcsncpy(out, pe.szExeFile, out_len - 1);
                out[out_len - 1] = 0;
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Шлях до overlay.dll поруч із самим інжектором, якщо явно не задано інший.
std::wstring default_dll_path() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    PathRemoveFileSpecW(self);
    std::wstring p = self;
    p += L"\\overlay.dll";
    return p;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    DWORD pid = 0;
    std::wstring dll_path;

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--exe") == 0 && i + 1 < argc) {
            pid = find_pid_by_exe(argv[++i]);
        } else if (wcscmp(argv[i], L"--dll") == 0 && i + 1 < argc) {
            dll_path = argv[++i];
        } else {
            wchar_t* end = NULL;
            unsigned long v = wcstoul(argv[i], &end, 10);
            if (end && *end == 0) pid = (DWORD)v;
        }
    }
    if (dll_path.empty()) dll_path = default_dll_path();

    if (!pid) {
        log("injector: процес не знайдено");
        fwprintf(stderr, L"process not found\n");
        return EX_NO_PROC;
    }

    wchar_t exe[MAX_PATH] = L"";
    exe_name_of(pid, exe, MAX_PATH);

    // Найперша перевірка — до всього іншого, навіть до наявності DLL. Відмова
    // для змагальної гри має бути безумовною: не «не зміг», а «не буду».
    wchar_t matched[MAX_PATH] = L"";
    hominka::GuardVerdict verdict = hominka::check_process(pid, exe, matched, MAX_PATH);
    if (verdict != hominka::GUARD_OK) {
        log("injector: ВІДМОВА для %ls (pid=%lu), причина=%d, збіг=%ls",
            exe, pid, (int)verdict, matched);
        fwprintf(stderr, L"refusing to inject into %ls (anti-cheat: %ls)\n", exe, matched);
        return EX_BLOCKED;
    }

    if (!PathFileExistsW(dll_path.c_str())) {
        log("injector: overlay.dll не знайдено поруч");
        fwprintf(stderr, L"overlay.dll not found: %ls\n", dll_path.c_str());
        return EX_ARGS;
    }

    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        log("injector: OpenProcess не вдалося, err=%lu (потрібні права адміністратора?)",
            GetLastError());
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return EX_INJECT;
    }

    bool bitness_ok = false;
    bool target64 = is_64bit(proc, &bitness_ok);
    if (bitness_ok && target64 != self_is_64bit()) {
        log("injector: розрядність не збігається (ціль %s, ми %s) — потрібен інший інжектор",
            target64 ? "x64" : "x86", self_is_64bit() ? "x64" : "x86");
        fwprintf(stderr, L"bitness mismatch: target is %ls, use the other injector\n",
                 target64 ? L"64-bit" : L"32-bit");
        CloseHandle(proc);
        return EX_BITNESS;
    }

    SIZE_T bytes = (wcslen(dll_path.c_str()) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        log("injector: VirtualAllocEx не вдалося, err=%lu", GetLastError());
        CloseHandle(proc);
        return EX_INJECT;
    }
    if (!WriteProcessMemory(proc, remote, dll_path.c_str(), bytes, NULL)) {
        log("injector: WriteProcessMemory не вдалося, err=%lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return EX_INJECT;
    }

    // LoadLibraryW у kernel32 — той самий адрес у всіх процесах цієї розрядності.
    LPTHREAD_START_ROUTINE loader =
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE thread = CreateRemoteThread(proc, NULL, 0, loader, remote, 0, NULL);
    if (!thread) {
        log("injector: CreateRemoteThread не вдалося, err=%lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return EX_INJECT;
    }

    WaitForSingleObject(thread, 10000);
    DWORD loaded = 0;
    GetExitCodeThread(thread, &loaded);   // нижні біти HMODULE; 0 = LoadLibrary впав
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (!loaded) {
        log("injector: LoadLibraryW у цілі повернув 0 — DLL не завантажилася");
        fwprintf(stderr, L"remote LoadLibrary failed\n");
        return EX_INJECT;
    }

    log("injector: overlay.dll вкладено в %ls (pid=%lu)", exe, pid);
    wprintf(L"injected into %ls (pid=%lu)\n", exe, pid);
    return EX_OK;
}
