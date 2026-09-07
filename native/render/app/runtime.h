// Дрібниці, потрібні всім режимам: журнал, час і ловець збоїв.
//
// Журнал пишеться у файл, а не в консоль: у вікна оверлея консолі немає, і
// коли він падає посеред гри, розповісти про це нікому. Той самий файл, що в
// dcomp_overlay.cpp, — щоб розбирати в одному місці.
#pragma once

#include <windows.h>

#include <cstdint>

namespace hominka {

// --verbose: докладний журнал розкладки. Вимкнений — рендер мовчить.
extern bool g_verbose;
// --dump-html: класти складену розмітку поруч зі знімком.
extern bool g_dump_html;
// --backdrop none: не підкладати темне тло під знімок самоперевірки.
extern bool g_backdrop;

void trace(const char* fmt, ...);
void rlog(const char* fmt, ...);

int64_t now_ms();

// Ловець збоїв: пише зсув від початку модуля разом із ланцюжком викликів.
LONG CALLBACK crash_veh(EXCEPTION_POINTERS* ep);

}  // namespace hominka
