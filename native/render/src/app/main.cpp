// Нативний рендер чату: розбір аргументів і більше нічого.
//
// Кожен режим живе у своєму файлі поруч; тут лишається тільки те, заради чого
// main і потрібен, — вирішити, який із них запускати. Доки все це було в
// одному файлі на півтори тисячі рядків, знайти в ньому цикл програми було
// важче, ніж написати наново.
//
// БЕЗ АРГУМЕНТІВ ЗАПУСКАЄТЬСЯ САМА ПРОГРАМА. Це не дрібниця: цей .exe тепер і
// є Hominka, і подвійний клац по ньому має відкривати чат, а не показувати
// довідку й зникати. Саме так і сталося у 3.0.0: людина клацала — вигулькувала
// консоль із переліком ключів і одразу закривалася.
//
//   --app                        сам собі програма: свій config.json, свої
//                                канали, вікно чату, налаштування й редактор
//                                теми. Python не потрібен (app/overlay.cpp).
//   --run <pid>                  той самий цикл, але керує ним Hominka на
//                                Python, а повідомлення йдуть каналом.
//   --preview <pid>              кадр у редактор CSS старої програми
//                                (app/preview.cpp).
//   --selftest вхід.json вихід.png [--width N]
//                                звірка з браузером (app/selftest.cpp).
//   --nettest <площадка> <канал> [сек]   живий чат у консоль.
//   --csslint <тема.css>         помилки теми рядками, придатними для скрипта.
//   --updatecheck [канал] [версія] [--download] [--install]
//   --verifyrelease <маніфест.json>      чому не сходиться підпис.
//   --probe                      чи жива зв'язка litehtml + Direct2D.
//
// Спільні прапорці: --verbose (докладний журнал), --dump-html (класти
// розмітку поруч зі знімком), --backdrop none (знімок без темного тла).

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/diag.h"
#include "app/nettest.h"
#include "app/overlay.h"
#include "app/preview.h"
#include "app/runtime.h"
#include "app/selftest.h"
#include "gfx/cssbits.h"

namespace hominka {
namespace {

void usage() {
    fwprintf(stderr,
             L"Використання:\n"
             L"  hominka-render-x64.exe --selftest <вхід.json> <вихід.png> [--width N]\n"
             L"  hominka-render-x64.exe --app                (сам собі програма)\n"
             L"  hominka-render-x64.exe --run <pid Hominka>\n"
             L"  hominka-render-x64.exe --preview <pid Hominka>\n"
             L"  hominka-render-x64.exe --nettest <площадка> <канал> [сек]\n"
             L"  hominka-render-x64.exe --csslint <тема.css>\n"
             L"  hominka-render-x64.exe --updatecheck [канал] [версія] [--download|--install]\n"
             L"  hominka-render-x64.exe --verifyrelease <маніфест.json>\n"
             L"  hominka-render-x64.exe --probe\n");
}

// Широкі символи у вузькі. Усе, що сюди потрапляє, — латиниця (назва
// площадки, канал, номер версії), тож зайвих турбот про кодування немає.
void narrow(const wchar_t* src, char* dst, int cap) {
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, cap - 1, nullptr, nullptr);
}

int run(int argc, wchar_t** argv) {
    AddVectoredExceptionHandler(1, hominka::crash_veh);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--verbose")) {
            hominka::g_verbose = true;
            hominka::g_draw_trace = true;
        } else if (!wcscmp(argv[i], L"--dump-html")) {
            hominka::g_dump_html = true;
        } else if (!wcscmp(argv[i], L"--backdrop") && i + 1 < argc &&
                   !wcscmp(argv[i + 1], L"none")) {
            hominka::g_backdrop = false;
        }
    }

    // Ключів немає — це звичайний запуск програми.
    if (argc < 2) {
        const int rc = hominka::run_overlay(0, /*standalone=*/true);
        CoUninitialize();
        return rc;
    }

    // Далі — режими командного рядка, і їм потрібна консоль. Програма зібрана
    // віконною (щоб подвійний клац не блимав чорним вікном), тож консоль треба
    // позичити в того, хто нас запустив.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
    }

    int rc = 1;
    if (argc >= 2 && !wcscmp(argv[1], L"--probe")) {
        hominka::g_draw_trace = true;
        rc = hominka::probe_litehtml();
    } else if (argc >= 4 && !wcscmp(argv[1], L"--nettest")) {
        char plat[32] = {0}, ch[128] = {0};
        hominka::narrow(argv[2], plat, sizeof plat);
        hominka::narrow(argv[3], ch, sizeof ch);
        rc = hominka::nettest(plat, ch, argc >= 5 ? _wtoi(argv[4]) : 20);
    } else if (argc >= 3 && !wcscmp(argv[1], L"--preview")) {
        rc = hominka::run_preview((DWORD)_wtoi(argv[2]));
    } else if (argc >= 3 && !wcscmp(argv[1], L"--run")) {
        rc = hominka::run_overlay((DWORD)_wtoi(argv[2]), /*standalone=*/false);
    } else if (argc >= 3 && !wcscmp(argv[1], L"--verifyrelease")) {
        rc = hominka::verify_release(argv[2]);
    } else if (argc >= 2 && !wcscmp(argv[1], L"--updatecheck")) {
        char ch[32] = "stable", pretend[32] = {0};
        bool fetch = false, put = false;
        if (argc >= 3 && argv[2][0] != L'-') hominka::narrow(argv[2], ch, sizeof ch);
        for (int i = 3; i < argc; ++i) {
            if (!wcscmp(argv[i], L"--download")) fetch = true;
            else if (!wcscmp(argv[i], L"--install")) { fetch = true; put = true; }
            else hominka::narrow(argv[i], pretend, sizeof pretend);
        }
        rc = hominka::update_check(ch, pretend, fetch, put);
    } else if (argc >= 3 && !wcscmp(argv[1], L"--csslint")) {
        rc = hominka::css_check(argv[2]);
    } else if (argc >= 2 && !wcscmp(argv[1], L"--app")) {
        rc = hominka::run_overlay(0, /*standalone=*/true);
    } else if (argc >= 4 && !wcscmp(argv[1], L"--selftest")) {
        int width = 430;                       // типова ширина вікна чату
        for (int i = 4; i + 1 < argc; ++i)
            if (!wcscmp(argv[i], L"--width")) width = _wtoi(argv[i + 1]);
        rc = hominka::selftest(argv[2], argv[3], width);
    } else {
        hominka::usage();
    }

    CoUninitialize();
    return rc;
}

}  // namespace
}  // namespace hominka

// Точка входу — WinMain, а не wmain, і це не примха.
//
// Програма зібрана віконною: подвійний клац не має блимати чорною консоллю.
// Віконна підсистема починає з WinMainCRTStartup, а той кличе WinMain. Спроба
// лишити wmain і переставити точку входу ключем -municode на цьому наборі
// компіляторів мовчки не спрацьовує: ld не знаходить wmainCRTStartup, ставить
// точкою входу початок коду — і .exe завершується нулем, нічого не зробивши.
// Тобто замість консолі, що блимала у 3.0.0, вийшло б вікно, яке взагалі
// не з'являється, — те саме «не відкривається», лише мовчки.
//
// Аргументи беремо з GetCommandLineW, а не з __wargv: останній заповнює
// юнікодний запуск CRT, якого тут якраз і немає.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const int rc = hominka::run(argc, argv);
    if (argv) LocalFree(argv);
    return rc;
}
