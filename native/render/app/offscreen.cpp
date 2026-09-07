#include "app/offscreen.h"

#include <cstdio>

#include "app/runtime.h"

namespace hominka {

// Підкладка під картинку самоперевірки. Чат білий і напівпрозорий — на білому
// тлі переглядача його просто не видно, і «текст зник» помилково читається як
// збій. Тому за замовчуванням кладемо його на темне, як він і живе поверх гри.
void apply_backdrop(std::vector<uint8_t>* bgra, uint8_t b, uint8_t g, uint8_t r) {
    for (size_t i = 0; i + 3 < bgra->size(); i += 4) {
        const unsigned a = (*bgra)[i + 3];
        if (a == 255) continue;
        const unsigned inv = 255 - a;
        // Пікселі premultiplied, тож підкладка просто домішується зверху.
        (*bgra)[i + 0] = (uint8_t)((*bgra)[i + 0] + b * inv / 255);
        (*bgra)[i + 1] = (uint8_t)((*bgra)[i + 1] + g * inv / 255);
        (*bgra)[i + 2] = (uint8_t)((*bgra)[i + 2] + r * inv / 255);
        (*bgra)[i + 3] = 255;
    }
}

bool save_png(IWICImagingFactory* wic, const wchar_t* path, int w, int h,
              const std::vector<uint8_t>& bgra) {
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = false;

    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, &props)) &&
        SUCCEEDED(frame->Initialize(props))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(frame->SetSize((UINT)w, (UINT)h)) &&
            SUCCEEDED(frame->SetPixelFormat(&fmt))) {
            // PNG зберігаємо straight (не premultiplied) — інакше переглядачі
            // показують напівпрозорі місця темнішими, ніж вони є.
            std::vector<uint8_t> straight = bgra;
            for (size_t i = 0; i + 3 < straight.size(); i += 4) {
                const unsigned a = straight[i + 3];
                if (a == 0 || a == 255) continue;
                for (int k = 0; k < 3; ++k) {
                    const unsigned v = straight[i + k] * 255u / a;
                    straight[i + k] = (uint8_t)(v > 255 ? 255 : v);
                }
            }
            if (SUCCEEDED(frame->WritePixels((UINT)h, (UINT)w * 4, (UINT)straight.size(),
                                             straight.data())) &&
                SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit()))
                ok = true;
        }
    }
    if (props) props->Release();
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    return ok;
}

// Знімок вікна у PNG. Потрібен саме в робочому режимі: вікно приховане від
// захоплення екрана, тож звичайний скриншот його не бачить.
bool dump_window_png(DCompWindow* win, const wchar_t* path);

std::string read_file(const wchar_t* path) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return "";
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

bool dump_window_png(DCompWindow* win, const wchar_t* path) {
    std::vector<uint8_t> px;
    if (!win->capture(&px)) return false;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))))
        return false;
    apply_backdrop(&px, 24, 20, 18);
    const bool ok = save_png(wic, path, win->width(), win->height(), px);
    wic->Release();
    return ok;
}

bool dump_gui_png(GuiWindow* gui, const wchar_t* path) {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (!gui->capture(&px, &w, &h)) return false;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))))
        return false;
    const bool ok = save_png(wic, path, w, h, px);
    wic->Release();
    return ok;
}

}  // namespace hominka
