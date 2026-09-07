// Те, що стрічці потрібно від рушія малювання — і більше нічого.
//
// Навіщо. Стрічка (feed.cpp) — це головна ідея всього рендера: рядок
// растеризується РАЗ і далі кладеться готовою картинкою. Мати дві копії цієї
// логіки, віконну й Linux-ову, означало б, що одного дня вони розійдуться —
// і розійдуться тихо, у поведінці, а не в компіляції.
//
// Тому стрічка одна, а від системи їй потрібно рівно шість дій:
//   зробити окреме полотно, почати й закінчити на ньому малювання, узяти з
//   нього растр, зробити растр із готових пікселів і покласти растр із альфою.
//
// Усе інше (текст, заливки, градієнти) стрічки не стосується — то робота
// контейнера, і там два різні файли, як і має бути.
#pragma once

#include <cstdint>

#ifdef _WIN32
#include <d2d1_1.h>
#include <dwrite.h>
#else
#include <blend2d.h>
#include "fontstore.h"
#endif

namespace hominka {

#ifdef _WIN32

using GfxFonts = IDWriteFactory;         // звідки беруться накреслення
using GfxTarget = ID2D1RenderTarget;     // куди малюють
using GfxSurface = ID2D1BitmapRenderTarget;   // окреме полотно
using GfxRaster = ID2D1Bitmap;           // готовий растр

#else

using GfxFonts = FontStore;
using GfxTarget = BLContext;
using GfxRaster = BLImage;

// Полотно рядка: картинка разом із контекстом, що на ній малює. Direct2D тримає
// це однією річчю (ID2D1BitmapRenderTarget), Blend2D — двома.
struct GfxSurface {
    BLImage img;
    BLContext ctx;
    BLImage* raster = nullptr;           // те саме, що img; віддаємо вказівником
};

#endif

// Полотно розміру w×h, сумісне з ціллю rt. nullptr — не вийшло.
GfxSurface* gfx_offscreen(GfxTarget* rt, int w, int h);
// Почати малювати на полотні (і очистити його прозорим).
GfxTarget* gfx_begin(GfxSurface* s);
// Закінчити. false — малювання не вдалося, полотном користуватися не можна.
bool gfx_end(GfxSurface* s);
// Растр полотна. Живе стільки ж, скільки саме полотно.
GfxRaster* gfx_raster(GfxSurface* s);
// Растр із готових пікселів (premultiplied BGRA, рядок = w*4).
GfxRaster* gfx_raster_from_bgra(GfxTarget* rt, const uint8_t* px, int w, int h);
// Покласти растр у прямокутник із заданою прозорістю.
void gfx_blit(GfxTarget* rt, GfxRaster* r, float left, float top,
              float right, float bottom, float alpha);

void gfx_release(GfxSurface* s);
void gfx_release(GfxRaster* r);

}  // namespace hominka
