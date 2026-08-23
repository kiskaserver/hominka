// Куди ми не полізаємо ніколи.
//
// Внутрішньоігровий чат — зручність, а не привід втратити акаунт. У змагальних
// іграх стороння бібліотека всередині процесу — це те, за що видають бан, і
// жоден античит не зобовʼязаний розбиратися, що саме ми там малюємо. Discord,
// OBS і Steam живуть у білих списках за домовленістю з розробниками античитів;
// у нас такої домовленості немає і не буде.
//
// Тому відмова тут — тверда. Не попередження, не галочка «я розумію ризик», а
// саме відмова: людина, яка вмикає оверлей, думає про чат, а не про Vanguard.
#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>

namespace hominka {

// Ігри, у яких відомо, що працює античит рівня ядра або клієнтський сканер
// памʼяті. Список неповний за визначенням — тому нижче ще й пошук самих
// античитів серед завантажених модулів.
inline const wchar_t* const* blocked_games(int* count) {
    static const wchar_t* names[] = {
        L"valorant.exe", L"valorant-win64-shipping.exe", L"vgc.exe", L"vgtray.exe",
        L"cs2.exe", L"csgo.exe",
        L"rustclient.exe",
        L"r5apex.exe", L"r5apex_dx12.exe",
        L"escapefromtarkov.exe", L"escapefromtarkov_be.exe",
        L"fortniteclient-win64-shipping.exe",
        L"pubg.exe", L"tslgame.exe",
        L"destiny2.exe",
        L"gta5.exe", L"gta5_enhanced.exe", L"rainbowsix.exe", L"rainbowsix_vulkan.exe",
        L"deltaforceclient-win64-shipping.exe",
        L"thefinals.exe", L"discovery.exe",
        L"battlefield2042.exe", L"bf2042.exe",
        L"cod.exe", L"modernwarfare.exe", L"blackopscoldwar.exe",
    };
    *count = (int)(sizeof(names) / sizeof(names[0]));
    return names;
}

// Самі античити. Якщо гра тягне за собою щось із цього — не наша справа, навіть
// якщо назви гри немає в списку вище.
inline const wchar_t* const* blocked_modules(int* count) {
    static const wchar_t* names[] = {
        L"easyanticheat.dll", L"easyanticheat_x64.dll", L"easyanticheat_eos.dll",
        L"beclient.dll", L"beclient_x64.dll", L"battleye.dll",
        L"vgk.sys", L"vgc.dll",
        L"faceitclient.dll", L"anticheat.dll", L"anticheatexpert.dll",
        L"ntdll_.dll",  // характерна підміна деяких сканерів
        L"xigncode3.dll", L"hshield.dll", L"gameguard.des",
    };
    *count = (int)(sizeof(names) / sizeof(names[0]));
    return names;
}

inline void lower_inplace(wchar_t* s) {
    for (; *s; ++s) *s = (wchar_t)towlower(*s);
}

// Чому саме відмовили — щоб інжектор міг сказати це людськими словами.
enum GuardVerdict {
    GUARD_OK = 0,
    GUARD_BLOCKED_GAME = 1,     // гра зі списку
    GUARD_BLOCKED_ANTICHEAT = 2 // усередині процесу знайдено античит
};

// Перевіряє процес за назвою і за завантаженими модулями.
// `matched` (може бути NULL) отримує те, через що саме відмовили.
inline GuardVerdict check_process(DWORD pid, const wchar_t* exe_name,
                                  wchar_t* matched, size_t matched_len) {
    wchar_t name[MAX_PATH];
    wcsncpy(name, exe_name ? exe_name : L"", MAX_PATH - 1);
    name[MAX_PATH - 1] = 0;
    lower_inplace(name);

    int n = 0;
    const wchar_t* const* games = blocked_games(&n);
    for (int i = 0; i < n; ++i) {
        if (wcscmp(name, games[i]) == 0) {
            if (matched) wcsncpy(matched, games[i], matched_len - 1);
            return GUARD_BLOCKED_GAME;
        }
    }

    // Другий рубіж: назви ігор змінюються, античити — рідше.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return GUARD_OK;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    GuardVerdict verdict = GUARD_OK;
    const wchar_t* const* mods = blocked_modules(&n);
    if (Module32FirstW(snap, &me)) {
        do {
            wchar_t mod[MAX_PATH];
            wcsncpy(mod, me.szModule, MAX_PATH - 1);
            mod[MAX_PATH - 1] = 0;
            lower_inplace(mod);
            for (int i = 0; i < n; ++i) {
                if (wcscmp(mod, mods[i]) == 0) {
                    if (matched) wcsncpy(matched, mods[i], matched_len - 1);
                    verdict = GUARD_BLOCKED_ANTICHEAT;
                    break;
                }
            }
        } while (verdict == GUARD_OK && Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return verdict;
}

}  // namespace hominka
