// Шість дій зі стрічкового заголовка — по разу на кожну систему.
#include "core/feedgfx.h"

#include <cstring>

namespace hominka {

#ifdef _WIN32

GfxSurface* gfx_offscreen(GfxTarget* rt, int w, int h) {
    if (!rt || w <= 0 || h <= 0) return nullptr;
    ID2D1BitmapRenderTarget* s = nullptr;
    if (FAILED(rt->CreateCompatibleRenderTarget(D2D1::SizeF((float)w, (float)h), &s)))
        return nullptr;
    return s;
}

GfxTarget* gfx_begin(GfxSurface* s) {
    if (!s) return nullptr;
    s->BeginDraw();
    s->Clear(D2D1::ColorF(0, 0, 0, 0));
    return s;
}

bool gfx_end(GfxSurface* s) {
    return s && SUCCEEDED(s->EndDraw());
}

GfxRaster* gfx_raster(GfxSurface* s) {
    if (!s) return nullptr;
    ID2D1Bitmap* b = nullptr;
    if (FAILED(s->GetBitmap(&b))) return nullptr;
    // Растр ПОЗИЧЕНИЙ: ним володіє полотно, і живе він рівно стільки ж.
    // GetBitmap додає посилання, тож зайве одразу віддаємо — інакше картинка
    // рядка не звільнялася б ніколи (у Blend2D власника два не буває, і
    // контракт мусить бути один на обидві системи).
    b->Release();
    return b;
}

GfxRaster* gfx_raster_from_bgra(GfxTarget* rt, const uint8_t* px, int w, int h) {
    if (!rt || !px || w <= 0 || h <= 0) return nullptr;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap* b = nullptr;
    if (FAILED(rt->CreateBitmap(D2D1::SizeU((UINT32)w, (UINT32)h), px,
                                (UINT32)(w * 4), &props, &b)))
        return nullptr;
    return b;
}

void gfx_blit(GfxTarget* rt, GfxRaster* r, float left, float top,
              float right, float bottom, float alpha) {
    if (!rt || !r) return;
    rt->DrawBitmap(r, D2D1::RectF(left, top, right, bottom), alpha,
                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void gfx_release(GfxSurface* s) { if (s) s->Release(); }
void gfx_release(GfxRaster* r) { if (r) r->Release(); }

#else

GfxSurface* gfx_offscreen(GfxTarget* rt, int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;
    GfxSurface* s = new GfxSurface();
    if (s->img.create(w, h, BL_FORMAT_PRGB32) != BL_SUCCESS) {
        delete s;
        return nullptr;
    }
    s->raster = &s->img;
    return s;
}

GfxTarget* gfx_begin(GfxSurface* s) {
    if (!s) return nullptr;
    if (s->ctx.begin(s->img) != BL_SUCCESS) return nullptr;
    // Полотно щойно створене або використовується вдруге — чистимо прозорим.
    s->ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    s->ctx.fill_all(BLRgba32(0x00000000));
    s->ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
    return &s->ctx;
}

bool gfx_end(GfxSurface* s) {
    return s && s->ctx.end() == BL_SUCCESS;
}

GfxRaster* gfx_raster(GfxSurface* s) {
    return s ? s->raster : nullptr;
}

GfxRaster* gfx_raster_from_bgra(GfxTarget* rt, const uint8_t* px, int w, int h) {
    if (!px || w <= 0 || h <= 0) return nullptr;
    BLImage* img = new BLImage();
    // Копіюємо: кадр анімації може бути викинутий із кеша раніше, ніж растр.
    if (img->create(w, h, BL_FORMAT_PRGB32) != BL_SUCCESS) {
        delete img;
        return nullptr;
    }
    BLImageData d;
    if (img->make_mutable(&d) != BL_SUCCESS) {
        delete img;
        return nullptr;
    }
    uint8_t* dst = (uint8_t*)d.pixel_data;
    for (int y = 0; y < h; ++y)
        memcpy(dst + (size_t)y * d.stride, px + (size_t)y * w * 4, (size_t)w * 4);
    return img;
}

void gfx_blit(GfxTarget* rt, GfxRaster* r, float left, float top,
              float right, float bottom, float alpha) {
    if (!rt || !r) return;
    const BLRectI src(0, 0, r->width(), r->height());
    const BLRect dst(left, top, right - left, bottom - top);
    if (alpha >= 0.999f) {
        rt->blit_image(dst, *r, src);
        return;
    }
    rt->save();
    rt->set_global_alpha(alpha);
    rt->blit_image(dst, *r, src);
    rt->restore();
}

void gfx_release(GfxSurface* s) { delete s; }
void gfx_release(GfxRaster* r) { delete r; }

#endif

}  // namespace hominka
