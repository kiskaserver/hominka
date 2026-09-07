// Самоперевірка: скласти стрічку зі зразків і зберегти PNG.
//
// Саме цим ми звіряємося з браузером — доки картинка не збігається,
// підключати рендер до програми немає сенсу.
#pragma once

#include <windows.h>

namespace hominka {

int selftest(const wchar_t* in_path, const wchar_t* out_path, int width);

// Чи жива зв'язка litehtml + Direct2D. Ділить навпіл: якщо падає і тут — річ у
// контейнері, а не в розмітці чату.
int probe_litehtml();

}  // namespace hominka
