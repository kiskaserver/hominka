#include "app/selftest.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/offscreen.h"
#include "app/runtime.h"
#include "core/chat_doc.h"
#include "core/feed.h"
#include "core/page_assets.h"
#include "gfx/container_d2d.h"
#include "gfx/imgcache.h"
#include "litehtml.h"

using json = nlohmann::json;

namespace hominka {

int selftest(const wchar_t* in_path, const wchar_t* out_path, int width) {
    const std::string text = read_file(in_path);
    if (text.empty()) {
        fwprintf(stderr, L"не читається вхідний файл: %s\n", in_path);
        return 2;
    }
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        fprintf(stderr, "JSON не розібрався: %s\n", e.what());
        return 2;
    }

    // Приймаємо і голий масив повідомлень, і об'єкт із налаштуваннями.
    json messages = j.is_array() ? j : (j.contains("messages") ? j["messages"] : json::array());
    std::string user_css;
    std::vector<std::string> layout;
    float zoom = 1.0f;
    bool animate = false;
    int64_t at_ms = -1;
    if (j.is_object()) {
        if (j.contains("css") && j["css"].is_string()) user_css = j["css"].get<std::string>();
        if (j.contains("layout") && j["layout"].is_array())
            for (const auto& s : j["layout"])
                if (s.is_string()) layout.push_back(s.get<std::string>());
        if (j.contains("zoom") && j["zoom"].is_number()) zoom = j["zoom"].get<float>();
        if (j.contains("width") && j["width"].is_number()) width = j["width"].get<int>();
        // Анімація вимкнена НАВМИСНО: знімок має виходити той самий від
        // запуску до запуску. Але перевірити саму анімацію теж треба — для
        // цього її вмикають і просять конкретну мить.
        if (j.contains("animate") && j["animate"].is_boolean())
            animate = j["animate"].get<bool>();
        if (j.contains("timeMs") && j["timeMs"].is_number())
            at_ms = (int64_t)j["timeMs"].get<double>();
    }

    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        fprintf(stderr, "WIC недоступний\n");
        return 3;
    }
    ID2D1Factory1* d2d = nullptr;
    D2D1_FACTORY_OPTIONS opt{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 &opt, (void**)&d2d))) {
        fprintf(stderr, "Direct2D недоступний\n");
        return 3;
    }
    IDWriteFactory* dwrite = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&dwrite))) {
        fprintf(stderr, "DirectWrite недоступний\n");
        return 3;
    }

    ImageCache images;
    Feed feed(dwrite, &images);
    // Самоперевірка має давати однаковий PNG від запуску до запуску, тож
    // анімовані емоути тут завмирають на першому кадрі — доки в зразку не
    // сказано інакше («animate»: true, «timeMs»: коли саме).
    feed.set_animate(animate);
    feed.set_width(width);
    feed.set_zoom(zoom);
    feed.set_layout(layout);
    feed.set_css(user_css);

    int count = 0;
    for (const auto& jm : messages) {
        if (!jm.is_object()) continue;
        const ChatMessage m = message_from_json(jm);
        if (g_dump_html) {
            const std::string html = message_document(m, layout, user_css);
            fwrite(html.data(), 1, html.size(), stdout);
        }
        feed.add(m, 0);
        ++count;
    }
    trace("повідомлень додано: %d", count);

    // Розкладка не потребує цілі — тож висоту полотна знаємо ДО того, як його
    // створимо. Заради цього розкладка й відділена від растеризації.
    const int total = feed.content_height();
    trace("висота вмісту: %d", total);
    if (count == 0 || total <= 0) {
        fprintf(stderr, "жодного повідомлення не намальовано\n");
        return 4;
    }

    WicTarget target;
    if (!target.create(wic, d2d, width, total)) {
        fprintf(stderr, "не створилася ціль малювання %dx%d\n", width, total);
        return 3;
    }
    // Час беремо завідомо більший за анімацію появи: знімок має показувати
    // усталений вигляд, а не випадковий кадр.
    const int64_t t = at_ms >= 0 ? at_ms : FEED_ENTER_MS * 10;
    target.rt()->BeginDraw();
    target.rt()->Clear(D2D1::ColorF(0, 0, 0, 0));
    feed.draw(target.rt(), width, total, t);
    const HRESULT hr = target.rt()->EndDraw();
    if (FAILED(hr)) {
        fprintf(stderr, "малювання не вдалося hr=0x%08lX\n", (unsigned long)hr);
        return 3;
    }

    std::vector<uint8_t> canvas;
    if (!target.read(width, total, &canvas)) {
        fprintf(stderr, "не прочиталися пікселі\n");
        return 3;
    }
    if (g_backdrop) apply_backdrop(&canvas, 24, 20, 18);

    if (!save_png(wic, out_path, width, total, canvas)) {
        fwprintf(stderr, L"не записався PNG: %s\n", out_path);
        return 5;
    }

    const std::vector<std::string> missing = feed.take_missing();
    printf("намальовано повідомлень: %d, полотно %dx%d, картинок у кеші: %d\n",
           count, width, total, (int)images.size());
    if (!missing.empty()) {
        printf("не знайшлося картинок: %d\n", (int)missing.size());
        for (size_t i = 0; i < missing.size() && i < 8; ++i)
            printf("  %s\n", missing[i].c_str());
    }
    return 0;
}

// Перевірка теми з командного рядка: те саме, що показує редактор, але у
// вигляді, придатному для перевірки скриптом. Розбір CSS тут свій і ручний —
// саме такі речі й ламаються тихо, тож пастки на них ганяються окремо
// (csslint_smoke.py).

// Найпростіша перевірка: розібрати тривіальну сторінку НАШИМ контейнером.
// Ділить навпіл: якщо падає і тут — річ у контейнері, а не в розмітці чату.
int probe_litehtml() {
    IDWriteFactory* dwrite = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&dwrite))) {
        fprintf(stderr, "DirectWrite недоступний\n");
        return 3;
    }
    ImageCache images;
    ContainerD2D container(dwrite, &images);
    container.begin(nullptr, 400, 100);
    fprintf(stderr, "проба: createFromString…\n");
    litehtml::document::ptr doc = litehtml::document::createFromString(
        "<html><body><span>привіт</span></body></html>", &container);
    fprintf(stderr, "проба: doc=%p\n", (void*)doc.get());
    if (!doc) return 4;
    fprintf(stderr, "проба: render…\n");
    doc->render(400);
    fprintf(stderr, "проба: висота %d\n", (int)doc->height());
    return 0;
}

}  // namespace hominka
