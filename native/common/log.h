// Журнал для того, що працює всередині чужого процесу.
//
// Ні консолі, ні вікна, ні винятків показати нікому: ми гість у грі, і єдине
// місце, куди можна чесно писати, — файл. Без нього налагодження зводиться до
// «не працює», бо навіть падіння виглядає як падіння ГРИ, а не наше.
#pragma once

#include <windows.h>
#include <stdio.h>

namespace hominka {

inline const wchar_t* log_path() {
    static wchar_t path[MAX_PATH] = {0};
    if (!path[0]) {
        wchar_t tmp[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (!n || n >= MAX_PATH) wcscpy(tmp, L".\\");
        _snwprintf(path, MAX_PATH, L"%shominka-overlay.log", tmp);
    }
    return path;
}

// Дописуємо, а не переписуємо: цікаве часто трапляється за запуск ДО того, на
// якому людина здалася і пішла скаржитися.
inline void log(const char* fmt, ...) {
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int head = _snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d pid=%lu tid=%lu] ",
                         t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                         GetCurrentProcessId(), GetCurrentThreadId());
    if (head < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(line + head, sizeof(line) - head - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)(sizeof(line) - head - 2);
    line[head + n] = '\n';
    line[head + n + 1] = 0;

    HANDLE f = CreateFileW(log_path(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, line, (DWORD)strlen(line), &written, NULL);
    CloseHandle(f);
}

}  // namespace hominka
