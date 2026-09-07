// Малювання повз екран і збереження PNG.
//
// Разом, бо це одна річ: намалювати кадр туди, де його ніхто не побачить, і
// покласти у файл. Потрібне двічі — самоперевірці (звірка з браузером) і
// предпросмотру для редактора CSS.
//
// Знімки вікон теж тут: обидва вікна інтерфейсу приховані від захоплення
// екрана, і звичайний скриншот їх не бачить — знімати доводиться просто з
// заднього буфера.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <wincodec.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/dcomp_window.h"
#include "ui/gui_win.h"

namespace hominka {

// Ціль малювання поверх WIC-картинки. У робочому режимі на її місце стає
// текстура свопчейна (common/dcomp_window.h), а код стрічки лишається тим
// самим — він працює з ID2D1RenderTarget.
class WicTarget {
public:
    WicTarget() = default;
    ~WicTarget() { reset(); }
    // Копіювати не можна: усередині COM-вказівники, і почленна копія лишила б
    // два власники на один об'єкт. Заборонено явно, бо помилка тиха — вона не
    // падає, а просто тече памʼяттю.
    WicTarget(const WicTarget&) = delete;
    WicTarget& operator=(const WicTarget&) = delete;

    void reset() {
        if (rt_) { rt_->Release(); rt_ = nullptr; }
        if (bitmap_) { bitmap_->Release(); bitmap_ = nullptr; }
    }

    bool create(IWICImagingFactory* wic, ID2D1Factory1* d2d, int w, int h) {
        reset();
        if (FAILED(wic->CreateBitmap((UINT)w, (UINT)h, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, &bitmap_)))
            return false;
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        return SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap_, props, &rt_));
    }
    ID2D1RenderTarget* rt() { return rt_; }
    bool read(int w, int h, std::vector<uint8_t>* out) {
        out->assign((size_t)w * h * 4, 0);
        return SUCCEEDED(bitmap_->CopyPixels(nullptr, (UINT)w * 4, (UINT)out->size(),
                                             out->data()));
    }

private:
    IWICBitmap* bitmap_ = nullptr;
    ID2D1RenderTarget* rt_ = nullptr;
};

std::string read_file(const wchar_t* path);

// Підкладає під напівпрозорий кадр темне тло — інакше на знімку не видно
// нічого, крім країв.
void apply_backdrop(std::vector<uint8_t>* bgra, uint8_t b, uint8_t g, uint8_t r);

bool save_png(IWICImagingFactory* wic, const wchar_t* path, int w, int h,
              const std::vector<uint8_t>& bgra);

// Знімок вікна оверлея й знімок вікна інтерфейсу. Обидва — з заднього буфера,
// ДО показу: у flip-моделі після Present буфер уже інший.
bool dump_window_png(DCompWindow* win, const wchar_t* path);
bool dump_gui_png(GuiWindow* gui, const wchar_t* path);

}  // namespace hominka
