#include "app/runtime.h"

#include <cstdarg>
#include <cstdio>
#include <string>

#include <chrono>

namespace hominka {

// Ловимо збої і пишемо зсув від початку модуля разом із ланцюжком викликів.
// Без цього окремий процес падав би без жодного сліду — а зсув лягає прямо в
// addr2line на нестрипнутій збірці й дає файл із рядком. Той самий підхід, що
// в dcomp_overlay.cpp.
LONG CALLBACK crash_veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // 0xE06D7363 — кидок C++ (bad_alloc тощо). Ловимо і його: інакше про
    // невдале виділення памʼяті ми дізнаємось лише з terminate, коли стек уже
    // розкручено й місце кидка втрачено. Друкуємо тільки перший.
    static LONG reported = 0;
    if (code == 0xC0000005 || code == 0xC0000409 || code == 0xC000001D ||
        (code == 0xE06D7363 && InterlockedExchange(&reported, 1) == 0)) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof name);
            fprintf(stderr, "КРАШ code=0x%lX модуль=%s зсув=0x%llX\n",
                    (unsigned long)code, name,
                    (unsigned long long)((char*)addr - (char*)mod));
        } else {
            fprintf(stderr, "КРАШ code=0x%lX addr=%p (модуль невідомий)\n",
                    (unsigned long)code, addr);
        }
        void* frames[40];
        const USHORT n = CaptureStackBackTrace(0, 40, frames, nullptr);
        fprintf(stderr, "ланцюжок викликів (%d):\n", (int)n);
        for (USHORT i = 0; i < n; ++i) {
            HMODULE m = nullptr;
            char mn[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)frames[i], &m) && m) {
                GetModuleFileNameA(m, mn, sizeof mn);
                const char* base = strrchr(mn, '\\');
                fprintf(stderr, "  [%02d] %s+0x%llX\n", (int)i, base ? base + 1 : mn,
                        (unsigned long long)((char*)frames[i] - (char*)m));
            } else {
                fprintf(stderr, "  [%02d] %p\n", (int)i, frames[i]);
            }
        }
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool g_verbose = false;
// --dump-html: вивести готову розмітку повідомлення у stdout. Потрібно, коли
// треба перевірити саме її, окремо від малювання.
bool g_dump_html = false;
// Підкладати темне тло під готовий PNG (--backdrop none вимикає).
bool g_backdrop = true;

void trace(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

// У робочому режимі консолі немає — пишемо в той самий журнал, що й решта
// оверлея (%TEMP%\hominka-overlay.log). Один файл на всі частини: коли щось
// не так, дивитися треба в одному місці, а не в трьох.
void rlog(const char* fmt, ...) {
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char path[MAX_PATH];
    const DWORD n = GetTempPathA(MAX_PATH, path);
    if (!n || n > MAX_PATH - 24) return;
    lstrcatA(path, "hominka-overlay.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] render: %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fclose(f);
}

int64_t now_ms() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (int64_t)(t.QuadPart * 1000 / freq.QuadPart);
}

}  // namespace hominka
