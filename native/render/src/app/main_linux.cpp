// Нативний рендер чату під Linux.
//
// Два режими:
//   --selftest — намалювати зразки в PNG. Той самий вхід, що й у віконної
//                версії, тож картинки можна класти поруч і звіряти.
//   --run      — робота: вікно поверх усього й канал до Hominka.
//   --preview  — те саме БЕЗ вікна: кадр іде пікселями назад у Hominka, і
//                вона показує його в редакторі CSS. Так предпросмотр
//                малюється тим самим рушієм, що й справжній чат.
//
// Кадру ВСЕРЕДИНІ гри (інжект) тут немає: overlay.dll — річ віконна, а на
// Linux цей шлях вимагав би окремого шару Vulkan/GL. Чат показується поверх
// гри звичайним вікном, і для більшості випадків цього досить.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <blend2d.h>
#include <nlohmann/json.hpp>

#include "app/nettest.h"
#include "core/chat_doc.h"
#include "core/config.h"
#include "core/feed.h"
#include "core/look.h"
#include "gfx/cssbits.h"
#include "gfx/fontstore.h"
#include "gfx/imgcache.h"
#include "net/chatnet.h"
#include "net/imgfetch.h"
#include "platform/ipc.h"
#include "platform/x11_window.h"
#include "ui/chrome_bl.h"
#include <SDL.h>

#include "ui/gui_win_sdl.h"
#include "ui/settings_ui.h"

using json = nlohmann::json;
using namespace hominka;

namespace {

bool g_verbose = false;

void trace(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

std::string get_str(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string()) return "";
    return j[key].get<std::string>();
}

int selftest(const char* in_path, const char* out_path) {
    std::ifstream f(in_path);
    if (!f) {
        fprintf(stderr, "не читається: %s\n", in_path);
        return 2;
    }
    std::stringstream ss;
    ss << f.rdbuf();

    json j;
    try {
        j = json::parse(ss.str());
    } catch (const std::exception& e) {
        fprintf(stderr, "вхід не розібрався: %s\n", e.what());
        return 2;
    }

    const int width = j.contains("width") && j["width"].is_number()
                          ? j["width"].get<int>() : 430;
    const float zoom = j.contains("zoom") && j["zoom"].is_number()
                           ? j["zoom"].get<float>() : 1.0f;
    const std::string user_css = get_str(j, "css");

    std::vector<std::string> layout;
    if (j.contains("layout") && j["layout"].is_array())
        for (const auto& v : j["layout"])
            if (v.is_string()) layout.push_back(v.get<std::string>());

    const json empty = json::array();
    const json& messages = j.contains("messages") && j["messages"].is_array()
                               ? j["messages"] : empty;

    FontStore fonts;
    if (!fonts.ok()) {
        fprintf(stderr, "FreeType недоступний\n");
        return 3;
    }
    ImageCache images;
    Feed feed(&fonts, &images);
    // Самоперевірка має давати однаковий PNG від запуску до запуску, тож
    // анімовані емоути тут завмирають на першому кадрі.
    feed.set_animate(false);
    feed.set_width(width);
    feed.set_zoom(zoom);
    feed.set_layout(layout);
    feed.set_css(user_css);

    int count = 0;
    for (const auto& jm : messages) {
        if (!jm.is_object()) continue;
        feed.add(message_from_json(jm), 0);
        ++count;
    }
    trace("повідомлень додано: %d", count);

    // Розкладка не потребує цілі — висоту полотна знаємо ДО того, як його
    // створимо. Заради цього розкладка й відділена від растеризації.
    const int total = feed.content_height();
    trace("висота вмісту: %d", total);
    if (count == 0 || total <= 0) {
        fprintf(stderr, "жодного повідомлення не намальовано\n");
        return 4;
    }

    BLImage canvas;
    if (canvas.create(width, total, BL_FORMAT_PRGB32) != BL_SUCCESS) {
        fprintf(stderr, "не створилася ціль малювання %dx%d\n", width, total);
        return 3;
    }
    BLContext ctx;
    if (ctx.begin(canvas) != BL_SUCCESS) {
        fprintf(stderr, "не почалося малювання\n");
        return 3;
    }
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.fill_all(BLRgba32(0x00000000));
    ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

    // Час беремо завідомо більший за анімацію появи: знімок має показувати
    // усталений вигляд, а не випадковий кадр.
    feed.draw(&ctx, width, total, FEED_ENTER_MS * 10);
    ctx.end();

    if (canvas.write_to_file(out_path) != BL_SUCCESS) {
        fprintf(stderr, "не записався PNG: %s\n", out_path);
        return 5;
    }
    printf("намальовано повідомлень: %d, полотно %dx%d, картинок у кеші: %d\n",
           count, width, total, (int)images.size());
    return 0;
}

// Коротка перевірка живучості: чи піднімається зв'язка litehtml + Blend2D +
// FreeType узагалі. Те саме, що --probe у віконній версії.
int probe() {
    printf("проба: FreeType і fontconfig…\n");
    FontStore fonts;
    if (!fonts.ok()) {
        fprintf(stderr, "FreeType не піднявся\n");
        return 1;
    }
    Face* face = fonts.face("sans-serif", 400, false, 20.0f);
    if (!face) {
        fprintf(stderr, "жодного шрифту не знайшлося\n");
        return 1;
    }
    printf("  шрифт: ascent=%.2f descent=%.2f ch=%.2f\n",
           face->ascent(), face->descent(), face->ch_width());
    const Glyph* g = face->glyph('A');
    printf("  гліф 'A': %dx%d, advance=%.2f\n", g ? g->width : -1,
           g ? g->height : -1, g ? g->advance : 0.0f);
    // Емодзі — окремо: саме заради них тут FreeType, а не растеризатор Blend2D.
    Face* emoji = fonts.fallback(0x1F600, 400, false, 20.0f);
    if (emoji) {
        const Glyph* e = emoji->glyph(0x1F600);
        printf("  емодзі: %dx%d, кольорова: %s\n", e ? e->width : -1,
               e ? e->height : -1, e && e->color ? "так" : "ні");
    } else {
        printf("  емодзі: шрифту не знайшлося\n");
    }

    printf("проба: createFromString…\n");
    ImageCache images;
    Feed feed(&fonts, &images);
    feed.set_width(300);
    ChatMessage m;
    m.platform = "twitch";
    m.name = "Проба";
    m.nick = "proba";
    m.text = "перевірка";
    feed.add(m, 0);
    const int h = feed.content_height();
    printf("проба: висота %d\n", h);
    return h > 0 ? 0 : 1;
}

// --- робочий режим --------------------------------------------------------

int64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Чи живий іще той, заради кого ми запустилися. Пішов — ідемо й ми: оверлей
// без Hominka нікому не потрібен і зняти його буде нічим.
bool parent_alive(pid_t pid) {
    return pid > 0 && kill(pid, 0) == 0;
}

struct RunState {
    int want_w = 430, want_h = 560;
    int want_x = 80, want_y = 80;
    bool enabled = true;
    bool bye = false;
    Look look;                        // прозорість, підкладка, кегль, замок
    std::string shot_path;
};

// Кадр із каналу. Повертає true, якщо стрічка чи вигляд змінилися.
bool apply(const IpcFrame& fr, Feed* feed, ImageCache* images, RunState* st) {
    json j;
    try {
        j = json::parse(fr.json);
    } catch (const std::exception& e) {
        trace("кадр не розібрався: %s", e.what());
        return false;
    }
    const std::string t = get_str(j, "t");

    if (t == "msg")    { feed->add(message_from_json(j), now_ms()); return true; }
    if (t == "delete") { feed->remove_id(get_str(j, "id")); return true; }
    if (t == "purge")  { feed->purge_nick(get_str(j, "nick")); return true; }
    if (t == "clear")  { feed->clear(); return true; }
    if (t == "css")    { feed->set_css(get_str(j, "css")); return true; }
    if (t == "bye")    { st->bye = true; return false; }
    if (t == "shot")   { st->shot_path = get_str(j, "path"); return true; }

    if (t == "layout") {
        std::vector<std::string> layout;
        if (j.contains("layout") && j["layout"].is_array())
            for (const auto& v : j["layout"])
                if (v.is_string()) layout.push_back(v.get<std::string>());
        feed->set_layout(layout);
        return true;
    }
    if (t == "enabled") {
        st->enabled = !j.contains("on") || !j["on"].is_boolean() || j["on"].get<bool>();
        return true;
    }
    if (t == "config") {
        if (j.contains("zoom") && j["zoom"].is_number())
            feed->set_zoom(j["zoom"].get<float>());
        if (j.contains("width") && j["width"].is_number())
            st->want_w = j["width"].get<int>();
        if (j.contains("height") && j["height"].is_number())
            st->want_h = j["height"].get<int>();
        if (j.contains("x") && j["x"].is_number()) st->want_x = j["x"].get<int>();
        if (j.contains("y") && j["y"].is_number()) st->want_y = j["y"].get<int>();
        if (j.contains("opacity") && j["opacity"].is_number())
            st->look.opacity = j["opacity"].get<float>();
        if (j.contains("bg_alpha") && j["bg_alpha"].is_number())
            st->look.bg_alpha = j["bg_alpha"].get<float>();
        if (j.contains("frameless") && j["frameless"].is_boolean())
            st->look.frameless = j["frameless"].get<bool>();
        if (j.contains("locked") && j["locked"].is_boolean())
            st->look.locked = j["locked"].get<bool>();
        if (j.contains("zoom") && j["zoom"].is_number())
            st->look.zoom = j["zoom"].get<float>();
        return true;
    }
    if (t == "image") {
        const std::string url = get_str(j, "url");
        if (url.empty() || fr.blob.empty()) return false;
        images->put(url, fr.blob.data(), fr.blob.size());
        feed->on_image_arrived(url);
        images->gc_animated(feed->animated_in_use());
        return true;
    }
    return false;
}

int run(pid_t parent_pid) {
    FontStore fonts;
    if (!fonts.ok()) {
        fprintf(stderr, "FreeType недоступний\n");
        return 3;
    }
    ImageCache images;
    Feed feed(&fonts, &images);
    images.set_evict_hook([&feed](const std::string& u) { feed.forget_image(u); });

    IpcServer ipc;
    if (!ipc.start((uint32_t)parent_pid)) {
        fprintf(stderr, "канал не створився\n");
        return 4;
    }
    fprintf(stderr, "чекаю на %s\n", ipc.name().c_str());

    RunState st;
    X11Window win;
    if (!win.create(st.want_x, st.want_y, st.want_w, st.want_h, "Hominka chat overlay")) {
        fprintf(stderr, "вікно не створилося (X-сервер? 32-бітний візуал?)\n");
        return 5;
    }
    feed.set_width(st.want_w);
    win.show();

    ChromeBL chrome;
    chrome.set_fonts(&fonts);

    // Полотно кадру. Робимо один раз на розмір: перестворювати його щокадру —
    // це те саме викидання памʼяті, від якого ми й пішли.
    BLImage canvas;
    BLContext ctx;
    int canvas_w = 0, canvas_h = 0;
    int64_t last_gc = 0;
    bool blanked = false;

    std::vector<IpcFrame> frames;
    for (;;) {
        if (!parent_alive(parent_pid)) break;

        frames.clear();
        ipc.poll(&frames);
        bool changed = false;
        for (const auto& fr : frames) changed |= apply(fr, &feed, &images, &st);
        if (st.bye) break;

        // Події — по одній: рішення про драг має ухвалюватися ПІСЛЯ кожної,
        // інакше рух, що прийшов разом із натисканням, обробиться раніше.
        X11Event ev;
        bool closed = false;
        while (win.poll_event(&ev)) {
            if (ev.closed) { closed = true; break; }

            // Спершу рамка (вона знає про кнопки), потім вікно.
            if (ev.motion) { chrome.on_motion(ev.mx, ev.my); changed = true; }
            if (ev.leave) { chrome.on_leave(); changed = true; }
            if (ev.press) {
                chrome.on_button(ev.mx, ev.my, true);
                switch (chrome.hit(ev.mx, ev.my, win.width(), win.height())) {
                case ChromeBL::Hit::Strip: win.start_drag(ev.mx, ev.my); break;
                case ChromeBL::Hit::Grip:  win.start_resize(ev.mx, ev.my); break;
                default: break;        // кнопка чи порожнє місце — рамці видніше
                }
                changed = true;
            }
            if (ev.release) { chrome.on_button(ev.mx, ev.my, false); changed = true; }

            if (ev.moved) {
                // Людина перетягнула вікно — правда про геометрію лишається в
                // Python, тож просто розповідаємо, що сталося.
                char buf[160];
                snprintf(buf, sizeof buf,
                         "{\"t\":\"geometry\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                         win.x(), win.y(), win.width(), win.height());
                ipc.send(buf);
                st.want_x = win.x();
                st.want_y = win.y();
                st.want_w = win.width();
                st.want_h = win.height();
                feed.set_width(st.want_w);
                changed = true;
            }
        }
        if (closed) break;

        if (st.want_w != win.width() || st.want_h != win.height() ||
            st.want_x != win.x() || st.want_y != win.y()) {
            win.set_geometry(st.want_x, st.want_y, st.want_w, st.want_h);
            feed.set_width(st.want_w);
            changed = true;
        }
        // Клік-крізь — лише коли рамка не чекає на мишу: інакше натискання на
        // її ж кнопку провалилося б у гру.
        win.set_click_through(st.look.locked && !chrome.wants_mouse());

        // Список бракуючих картинок забираємо, але НЕ шлемо: Hominka качає їх
        // сама, наперед (hominka/imagefetch.py), і слухача для такого запиту в
        // неї немає. Забрати треба однаково — інакше він ріс би без кінця.
        feed.take_missing();

        const int64_t t = now_ms();
        if (!st.enabled) {
            if (!blanked) { win.present_blank(); blanked = true; }
            usleep(16000);
            continue;
        }
        blanked = false;

        if (changed || feed.dirty(t) || !st.shot_path.empty()) {
            const int w = win.width(), h = win.height();
            if (w != canvas_w || h != canvas_h) {
                if (canvas_w) ctx.end();
                if (canvas.create(w, h, BL_FORMAT_PRGB32) != BL_SUCCESS) {
                    usleep(100000);
                    continue;
                }
                canvas_w = w;
                canvas_h = h;
            }
            if (ctx.begin(canvas) != BL_SUCCESS) { usleep(100000); continue; }
            ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
            ctx.fill_all(BLRgba32(0x00000000));
            ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
            if (st.look.opacity < 0.999f) ctx.set_global_alpha(st.look.opacity);
            // Порядок той самий, що й у віконної рамки: підкладка, чат, керування.
            chrome.draw_backdrop(&ctx, w, h, st.look);
            feed.draw(&ctx, w, h, t);
            const float was_zoom = st.look.zoom;
            const ChromeEvents ce = chrome.draw_controls(&ctx, w, h, &st.look);
            ctx.end();

            if (st.look.zoom != was_zoom) feed.set_zoom(st.look.zoom);
            // Правда про налаштування лишається в Python: ми лише кажемо, що
            // сталося, тими самими кадрами, що й віконна рамка.
            if (ce.look_changed) {
                char buf[160];
                snprintf(buf, sizeof buf,
                         "{\"t\":\"look\",\"opacity\":%.3f,\"bg_alpha\":%.3f,"
                         "\"zoom\":%.3f}",
                         st.look.opacity, st.look.bg_alpha, st.look.zoom);
                ipc.send(buf);
            }
            if (ce.lock_changed) {
                char buf[64];
                snprintf(buf, sizeof buf, "{\"t\":\"lock\",\"on\":%s}",
                         st.look.locked ? "true" : "false");
                ipc.send(buf);
            }
            if (ce.open_settings) ipc.send("{\"t\":\"settings\"}");

            BLImageData data;
            if (canvas.get_data(&data) == BL_SUCCESS)
                win.present((const uint8_t*)data.pixel_data, w, h);

            if (!st.shot_path.empty()) {
                canvas.write_to_file(st.shot_path.c_str());
                st.shot_path.clear();
            }
        }

        if (t - last_gc > 1000) {
            last_gc = t;
            images.gc_animated(feed.animated_in_use());
            feed.trim_anim();
        }
        usleep(16000);
    }

    if (canvas_w) ctx.end();
    win.destroy();
    ipc.stop();
    return 0;
}

// --- предпросмотр для редактора CSS ---------------------------------------
//
// Вікна тут немає навмисно: кадр малюється в память і їде назад каналом. Свій
// канал («-preview»), тож із вікном оверлея вони не перетинаються ніде —
// предпросмотр не може ані підмінити його, ані завалити.
int preview(pid_t parent_pid) {
    FontStore fonts;
    if (!fonts.ok()) {
        fprintf(stderr, "предпросмотр: FreeType недоступний\n");
        return 3;
    }
    ImageCache images;
    Feed feed(&fonts, &images);
    images.set_evict_hook([&feed](const std::string& u) { feed.forget_image(u); });

    IpcServer ipc;
    if (!ipc.start((uint32_t)parent_pid, "-preview")) {
        fprintf(stderr, "предпросмотр: канал не створився\n");
        return 4;
    }
    fprintf(stderr, "предпросмотр: чекаю на %s\n", ipc.name().c_str());

    RunState st;
    st.want_w = 360;
    st.want_h = 480;
    feed.set_width(st.want_w);

    BLImage canvas;
    BLContext ctx;
    int have_w = 0, have_h = 0;
    int64_t last_gc = 0;
    std::vector<IpcFrame> frames;

    for (;;) {
        if (!parent_alive(parent_pid)) break;

        frames.clear();
        ipc.poll(&frames);
        bool changed = false;
        for (const auto& fr : frames) changed |= apply(fr, &feed, &images, &st);
        if (st.bye) break;
        feed.take_missing();

        const int64_t t = now_ms();
        if (!changed && !feed.dirty(t)) { usleep(16000); continue; }

        if (st.want_w != have_w || st.want_h != have_h) {
            if (canvas.create(st.want_w, st.want_h, BL_FORMAT_PRGB32) != BL_SUCCESS) {
                usleep(200000);
                continue;
            }
            have_w = st.want_w;
            have_h = st.want_h;
            feed.set_width(have_w);
        }
        if (ctx.begin(canvas) != BL_SUCCESS) { usleep(50000); continue; }
        ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
        ctx.fill_all(BLRgba32(0x00000000));
        ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
        feed.draw(&ctx, have_w, have_h, t);
        ctx.end();

        if (t - last_gc > 1000) {
            last_gc = t;
            images.gc_animated(feed.animated_in_use());
            feed.trim_anim();
        }

        BLImageData data;
        if (canvas.get_data(&data) != BL_SUCCESS) { usleep(50000); continue; }
        // Рядок у Blend2D може бути довшим за ширину*4 (вирівнювання), а той
        // бік чекає щільні пікселі — тож при потребі складаємо рядок за рядком.
        const size_t tight = (size_t)have_w * 4;
        std::vector<uint8_t> px;
        px.resize(tight * (size_t)have_h);
        const uint8_t* src = (const uint8_t*)data.pixel_data;
        for (int y = 0; y < have_h; ++y)
            memcpy(px.data() + tight * (size_t)y, src + (size_t)y * data.stride, tight);

        json j;
        j["t"] = "frame";
        j["w"] = have_w;
        j["h"] = have_h;
        ipc.send(j.dump(), px.data(), px.size());
        usleep(16000);
    }
    ipc.stop();
    return 0;
}

// --- сам собі програма ------------------------------------------------------
//
// Те саме, що робить app/overlay.cpp під Windows, тільки коротше: Python тут
// більше ні до чого — канали, картинки й налаштування веде сам рендер.
//
// Чого ще немає: вікна налаштувань і редактора теми. Вони написані на ImGui, а
// офіційного бекенда під X11 у ImGui немає, і це наступний крок. Доти канали
// правляться в config.json (~/.config/hominka/config.json), а все, що є у
// смужці вікна, працює як і має.

// Події з площадок → стрічка.
bool pump_chat(ChatNet* net, Feed* feed, ImageCache* images, ImageFetch* fetch, int64_t t) {
    bool changed = false;
    ChatEvent ev;
    // Не більше жмені за кадр: на бурхливому каналі суцільний потік інакше
    // з'їв би кадр цілком.
    for (int i = 0; i < 32 && net->take(&ev); ++i) {
        switch (ev.type) {
        case ChatEvent::Type::Message:
            for (const auto& e : ev.msg.emotes)
                if (!e.url.empty() && !images->known(e.url)) fetch->want(e.url);
            for (const auto& b : ev.msg.badge_icons)
                if (!b.url.empty() && !images->known(b.url)) fetch->want(b.url);
            feed->add(ev.msg, t);
            break;
        case ChatEvent::Type::Delete: feed->remove_id(ev.id); break;
        case ChatEvent::Type::Purge: feed->purge_nick(ev.nick); break;
        }
        changed = true;
    }
    return changed;
}

// Докачані картинки → кеш. Розбираємо саме тут, у потоці малювання: кеш
// спільний із розкладкою, і робити його потокобезпечним заради кількох емоутів
// на секунду означало б платити блокуванням у найгарячішому місці.
bool pump_images(ImageFetch* fetch, ImageCache* images, Feed* feed) {
    bool changed = false;
    std::string url;
    std::vector<uint8_t> data;
    for (int i = 0; i < 8 && fetch->take(&url, &data); ++i) {
        images->put(url, data.data(), data.size());
        feed->on_image_arrived(url);
        changed = true;
    }
    return changed;
}

Motion motion_of(const std::string& name) {
    if (name == "freeze") return Motion::Freeze;
    if (name == "hide") return Motion::Hide;
    return Motion::Play;
}

// Стан джерел очима панелі. Те саме, що робить app/overlay.cpp; глядачів тут
// поки немає — лічильник під Linux ще не під'єднаний.
std::vector<SourceView> source_view(const ChatNet& net) {
    std::vector<SourceView> out;
    for (const ChatNet::SourceInfo& s : net.sources()) {
        SourceView v;
        v.name = s.name;
        v.configured = s.configured;
        v.connected = s.connected;
        v.note = s.note;
        out.push_back(v);
    }
    return out;
}

int run_app() {
    FontStore fonts;
    if (!fonts.ok()) {
        fprintf(stderr, "FreeType недоступний\n");
        return 3;
    }

    Config cfg;
    cfg.load();

    ImageCache images;
    Feed feed(&fonts, &images);
    images.set_evict_hook([&feed](const std::string& u) { feed.forget_image(u); });

    ImageFetch fetch;
    ChatNet net;

    Look look = cfg.look;
    feed.set_zoom(look.zoom);
    feed.set_css(cfg.custom_css);
    if (!cfg.layout.empty()) feed.set_layout(cfg.layout);
    images.set_motion(motion_of(cfg.motion));
    fetch.start();
    net.apply(cfg);

    X11Window win;
    if (!win.create(cfg.x, cfg.y, cfg.w, cfg.h, "Hominka chat overlay")) {
        fprintf(stderr, "вікно не створилося (X-сервер? 32-бітний візуал?)\n");
        return 5;
    }
    feed.set_width(cfg.w);
    win.show();
    trace("самостійний режим: %s", net.status().c_str());

    ChromeBL chrome;
    chrome.set_fonts(&fonts);

    GuiWindowSDL gui;
    SettingsState sstate;

    BLImage canvas;
    BLContext ctx;
    int canvas_w = 0, canvas_h = 0;
    int64_t last_gc = 0;
    bool first = true;

    for (;;) {
        // Події SDL — одні на весь процес, і розбирає їх один виклик. Вікно
        // чату на них не тримається: воно на голому X11.
        GuiWindowSDL::pump();

        bool changed = false;
        const int64_t t = now_ms();
        changed |= pump_chat(&net, &feed, &images, &fetch, t);
        changed |= pump_images(&fetch, &images, &feed);
        // Картинки, про які стрічка дізналася вже під час розкладки (тло з
        // теми, наприклад): під Windows їх забирає той самий цикл, тут теж.
        for (const std::string& u : feed.take_missing())
            if (!images.known(u)) fetch.want(u);

        // Події миші — не більше ОДНОГО натискання чи відпускання за коло.
        //
        // Рамка рахує натискання по парі «натиснули на кнопці — відпустили на
        // ній же», і кожну половину має побачити окремий кадр. Якщо вичерпати
        // чергу цілком, швидкий клац (а такий дає й тачпад, і будь-яка
        // автоматика) прийде обома половинами в одне коло — і зникне безслідно.
        X11Event ev;
        bool closed = false;
        while (win.poll_event(&ev)) {
            if (ev.closed) { closed = true; break; }
            if (ev.motion) { chrome.on_motion(ev.mx, ev.my); changed = true; }
            if (ev.leave) { chrome.on_leave(); changed = true; }
            if (ev.moved) {
                cfg.x = win.x();
                cfg.y = win.y();
                cfg.w = win.width();
                cfg.h = win.height();
                cfg.save();
                feed.set_width(win.width());
                changed = true;
            }
            if (ev.press) {
                chrome.on_button(ev.mx, ev.my, true);
                // Куди саме натиснули, знає рамка: смужка — тягнути вікно,
                // куточок — розтягувати, решта — її власні кнопки.
                switch (chrome.hit(ev.mx, ev.my, win.width(), win.height())) {
                case ChromeBL::Hit::Strip: win.start_drag(ev.mx, ev.my); break;
                case ChromeBL::Hit::Grip:  win.start_resize(ev.mx, ev.my); break;
                default: break;
                }
                changed = true;
                break;
            }
            if (ev.release) {
                chrome.on_button(ev.mx, ev.my, false);
                win.end_drag();
                changed = true;
                break;
            }
        }
        if (closed) break;

        // Клік-крізь — ЛИШЕ коли вікно замкнене.
        //
        // Під Windows правило інше (крізь, поки на вікно не навели), і воно там
        // працює, бо курсор там опитується щокадру. Тут наведення приходить
        // ПОДІЄЮ від X11 — а порожня вхідна область означає, що подій більше
        // не буде: вікно назавжди лишилося б без рамки. Саме це й сталося на
        // першій перевірці.
        win.set_click_through(look.locked && !chrome.wants_mouse());

        if (changed || first || feed.dirty(t) || chrome.hovered()) {
            first = false;
            const int w = win.width(), h = win.height();
            if (w != canvas_w || h != canvas_h) {
                if (canvas_w) ctx.end();
                if (canvas.create(w, h, BL_FORMAT_PRGB32) != BL_SUCCESS) {
                    usleep(100000);
                    continue;
                }
                canvas_w = w;
                canvas_h = h;
            }
            if (ctx.begin(canvas) != BL_SUCCESS) { usleep(100000); continue; }
            ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
            ctx.fill_all(BLRgba32(0x00000000));
            ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

            feed.set_alpha(look.opacity);
            chrome.draw_backdrop(&ctx, w, h, look);
            feed.draw(&ctx, w, h, t);
            const float was_zoom = look.zoom;
            const ChromeEvents ce = chrome.draw_controls(&ctx, w, h, &look);
            ctx.end();
            if (look.zoom != was_zoom) feed.set_zoom(look.zoom);

            // Правда про налаштування тепер тут, а не в Python: те, що покрутили
            // у смужці, лягає в той самий config.json.
            if (ce.look_changed || ce.lock_changed) {
                cfg.look = look;
                cfg.save();
            }
            if (ce.close) break;
            if (ce.open_settings) {
                trace("відкриваю налаштування");
                if (!gui.created() && !gui.create("Hominka — налаштування", 760, 560))
                    trace("вікно налаштувань НЕ створилося: %s", SDL_GetError());
                gui.show_beside(win.x(), win.y(), win.width(), win.height());
            }
            if (ce.geometry_changed) {
                cfg.x = win.x();
                cfg.y = win.y();
                cfg.w = win.width();
                cfg.h = win.height();
                cfg.save();
            }

            BLImageData data;
            if (canvas.get_data(&data) == BL_SUCCESS)
                win.present((const uint8_t*)data.pixel_data, w, h);
        }

        // Панель налаштувань. Своє вікно, свій контекст ImGui, свій кадр — з
        // вікном чату вона ділить лише config.
        if (gui.begin()) {
            UpdateView upd;                 // оновлювач під Linux ще не зроблено
            GameView game;                  // чат усередині гри — теж
            const SettingsEvents sev =
                draw_settings(&sstate, &cfg, source_view(net), upd, game,
                              "Linux · нативний рендер", gui.width(), gui.height());
            gui.end();
            gui.drag(sev.title_active);
            if (sev.close) gui.hide();
            if (sev.look_changed) {
                look = cfg.look;
                feed.set_zoom(look.zoom);
                changed = true;
            }
            if (sev.sources_changed) net.apply(cfg);
            if (sev.motion_changed) {
                images.set_motion(motion_of(cfg.motion));
                changed = true;
            }
            if (sev.changed) cfg.save();
        }

        // Прибирання анімованих емоутів — раз на секунду.
        if (t - last_gc > 1000) {
            last_gc = t;
            images.gc_animated(feed.animated_in_use());
            feed.trim_anim();
        }

        cfg.flush();
        usleep(16000);
    }

    cfg.x = win.x();
    cfg.y = win.y();
    cfg.w = win.width();
    cfg.h = win.height();
    cfg.look = look;
    cfg.save();
    cfg.flush(true);
    net.stop();
    fetch.stop();
    return 0;
}

void usage() {
    fprintf(stderr,
            "hominka-render-linux — нативний рендер чату\n"
            "  --selftest <вхід.json> <вихід.png>   намалювати зразки\n"
            "  --run <pid Hominka>                  вікно оверлея й канал\n"
            "  --preview <pid Hominka>              кадр у редактор CSS\n"
            "  --nettest <площадка> <канал> [сек]   прочитати живий чат\n"
            "  --probe                              перевірити зв'язку\n"
            "  --verbose                            докладний журнал\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<const char*> args;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--verbose")) { g_verbose = true; g_draw_trace = true; continue; }
        args.push_back(argv[i]);
    }
    // Без ключів — це звичайний запуск програми, як і під Windows.
    if (args.empty()) return run_app();
    if (!strcmp(args[0], "--app")) return run_app();

    if (!strcmp(args[0], "--probe")) return probe();
    if (!strcmp(args[0], "--nettest")) {
        if (args.size() < 3) { usage(); return 1; }
        return nettest(args[1], args[2], args.size() > 3 ? atoi(args[3]) : 20);
    }
    if (!strcmp(args[0], "--run")) {
        if (args.size() < 2) { usage(); return 1; }
        return run((pid_t)atoi(args[1]));
    }
    if (!strcmp(args[0], "--preview")) {
        if (args.size() < 2) { usage(); return 1; }
        return preview((pid_t)atoi(args[1]));
    }
    if (!strcmp(args[0], "--selftest")) {
        if (args.size() < 3) { usage(); return 1; }
        return selftest(args[1], args[2]);
    }
    usage();
    return 1;
}
