// Вікно чату: цикл, який і є програмою.
//
// Один цикл на два режими. standalone — програма сама собі: читає свій
// config.json, під'єднується до площадок, качає картинки, показує панель
// налаштувань і редактор теми. Інакше — тим керує Hominka на Python, а ми
// малюємо те, що прийшло каналом.
#pragma once

#include <windows.h>

namespace hominka {

int run_overlay();

}  // namespace hominka
