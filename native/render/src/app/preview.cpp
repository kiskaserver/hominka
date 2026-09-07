#include "app/preview.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/ipc_mode.h"
#include "app/offscreen.h"
#include "app/runtime.h"
#include "core/feed.h"
#include "core/look.h"
#include "gfx/container_d2d.h"
#include "gfx/imgcache.h"
#include "platform/ipc.h"

using json = nlohmann::json;

namespace hominka {

int run_preview(DWORD parent_pid) {
    rlog("предпросмотр: старт, батько pid=%lu", (unsigned long)parent_pid);
    HANDLE parent = parent_pid ? OpenProcess(SYNCHRONIZE, FALSE, parent_pid) : nullptr;

    IWICImagingFactory* wic = nullptr;
    ID2D1Factory1* d2d = nullptr;
    IDWriteFactory* dwrite = nullptr;
    D2D1_FACTORY_OPTIONS opt{};
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 &opt, (void**)&d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&dwrite))) {
        rlog("предпросмотр: графіка недоступна — вихід");
        return 3;
    }

    ImageCache images;
    Feed feed(dwrite, &images);
    // Викинули картинку — стрічка має забути її кадрові текстури, інакше
    // тримала б посилання на те, чого вже немає.
    images.set_evict_hook([&feed](const std::string& u) { feed.forget_image(u); });
    int64_t last_gc = 0;
    IpcServer ipc;
    if (!ipc.start(parent_pid, "-preview")) {
        rlog("предпросмотр: канал не створився — вихід");
        return 4;
    }
    rlog("предпросмотр: чекаю на %s", ipc.name().c_str());

    Look look;
    InjectState inject;                 // тут не вживається, але apply_frame його просить
    int want_w = 360, want_h = 480;
    bool enabled = true, bye = false;
    std::string shot_path;
    std::vector<IpcFrame> frames;
    std::vector<uint8_t> px;

    // Ціль малювання переживає кадри: перестворюємо лише коли змінився розмір.
    WicTarget target;
    int have_w = 0, have_h = 0;

    for (;;) {
        if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) {
            rlog("предпросмотр: Hominka зникла — виходжу");
            return 0;
        }
        frames.clear();
        ipc.poll(&frames);
        bool changed = false;
        for (const auto& fr : frames)
            changed |= apply_frame(fr, &feed, &images, &look, &inject, &want_w, &want_h,
                                   &enabled, &bye, &shot_path);
        if (bye) { rlog("предпросмотр: завершуюсь"); return 0; }

        const int64_t t = now_ms();
        if (!changed && !feed.dirty(t)) { Sleep(16); continue; }

        if (want_w != have_w || want_h != have_h) {
            if (!target.create(wic, d2d, want_w, want_h)) {
                rlog("предпросмотр: ціль %dx%d не створилася", want_w, want_h);
                Sleep(200);
                continue;
            }
            have_w = want_w;
            have_h = want_h;
        }

        target.rt()->BeginDraw();
        target.rt()->Clear(D2D1::ColorF(0, 0, 0, 0));
        feed.draw(target.rt(), have_w, have_h, t);
        // Прибирання анімованих емоутів — за розкладом, а не лише коли приїхала
        // нова картинка. Раз на секунду й одразу після draw(): саме там щойно
        // проставлено, які рядки видно, а які пішли за край.
        if (t - last_gc > 1000) {
            last_gc = t;
            const size_t gone = images.gc_animated(feed.animated_in_use());
            // І текстури кадрів — тих емоутів, яких зараз не видно.
            feed.trim_anim();
            if (gone)
                rlog("кеш: викинуто %u анімованих, лишилося %u на %u КБ",
                     (unsigned)gone, (unsigned)images.animated_count(),
                     (unsigned)(images.animated_bytes() / 1024));
        }
        if (FAILED(target.rt()->EndDraw())) { Sleep(50); continue; }
        if (!target.read(have_w, have_h, &px)) { Sleep(50); continue; }

        json j;
        j["t"] = "frame";
        j["w"] = have_w;
        j["h"] = have_h;
        ipc.send(j.dump(), px.data(), px.size());
        Sleep(16);
    }
}

}  // namespace hominka
