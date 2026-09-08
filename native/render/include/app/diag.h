// Діагностика з командного рядка: чому не сходиться тема, чому не
// встановлюється оновлення.
//
// Усе це видно і в самій програмі, але у вигляді, придатному для ока, а не для
// скрипта. Тут — рівно навпаки: рядки, які легко перевірити машиною
// (tools/csslint_smoke.py, tools/release_smoke.py).
#pragma once

#include <windows.h>

namespace hominka {

int css_check(const wchar_t* path);
int verify_release(const wchar_t* path);
// put — не лише завантажити, а й поставити: запустити підмінника й вийти.
int update_check(const char* channel, const char* pretend, bool fetch, bool put);

}  // namespace hominka
