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

#include "chat_doc.h"
#include "cssbits.h"
#include "feed.h"
#include "fontstore.h"
#include "imgcache.h"
#include "chrome_bl.h"
#include "ipc.h"
#include "net_probe.h"
#include "look.h"
#include "x11_window.h"

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

// Перша перевірка мережі в C++ — див. net_probe.h.
int nettest(const std::string& channel, int seconds) {
    printf("під'єднуюся до #%s на %d с…\n", channel.c_str(), seconds);
    const int n = net_probe_twitch(channel, seconds, [](const ProbeMessage& m) {
        printf("  %-20s %s\n", m.name.c_str(), m.text.c_str());
    });
    if (n < 0) {
        fprintf(stderr, "не під'єдналося\n");
        return 1;
    }
    printf("прочитано повідомлень: %d\n", n);
    return 0;
}

void usage() {
    fprintf(stderr,
            "hominka-render-linux — нативний рендер чату\n"
            "  --selftest <вхід.json> <вихід.png>   намалювати зразки\n"
            "  --run <pid Hominka>                  вікно оверлея й канал\n"
            "  --preview <pid Hominka>              кадр у редактор CSS\n"
            "  --nettest <канал> [секунд]           прочитати чат Twitch\n"
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
    if (args.empty()) { usage(); return 1; }

    if (!strcmp(args[0], "--probe")) return probe();
    if (!strcmp(args[0], "--nettest")) {
        if (args.size() < 2) { usage(); return 1; }
        return nettest(args[1], args.size() > 2 ? atoi(args[2]) : 20);
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
