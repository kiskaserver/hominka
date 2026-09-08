// Нативний рендер чату під Linux.
//
// Режими:
//   (без ключів) — сама програма: конфіг, канали, вікно чату, налаштування й
//                редактор теми.
//   --selftest — намалювати зразки в PNG. Той самий вхід, що й у віконної
//                версії, тож картинки можна класти поруч і звіряти.
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
#include "core/version.h"
#include "core/feed.h"
#include "core/look.h"
#include "gfx/cssbits.h"
#include "gfx/fontstore.h"
#include "gfx/imgcache.h"
#include "net/chatnet.h"
#include "net/imgfetch.h"
#include "platform/x11_window.h"
#include "ui/chrome_bl.h"
#include <SDL.h>

#include "update/update_view.h"
#include "update/updater.h"

#include "ui/cssedit_ui.h"
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

// Налаштування «анімовані емоути» → режим кеша картинок.
Motion motion_of(const std::string& name) {
    if (name == "freeze") return Motion::Freeze;
    if (name == "hide") return Motion::Hide;
    return Motion::Play;
}

// Перевірка оновлення з командного рядка.
//
// Те саме, що «--updatecheck» під Windows, і потрібне з тієї самої причини:
// підмінник — єдина частина оновлення, якої не видно ні з коду, ні з журналу,
// доки вона не спрацює. Тут ще й підпис перевіряється проти СПРАВЖНЬОГО
// маніфесту: формат того, що підписується, мусить збігатися з Python до байта.
int update_check(const char* channel, const char* pretend, bool fetch, bool put) {
    Updater up;
    const char* current = pretend && *pretend ? pretend : HOMINKA_VERSION;
    up.check(channel, current, "");
    for (int i = 0; i < 300 && up.state() == Updater::State::Checking; ++i) usleep(100000);

    if (up.state() == Updater::State::UpToDate) {
        printf("оновлень немає (у нас %s)\n", current);
        return 0;
    }
    if (up.state() != Updater::State::Available) {
        fprintf(stderr, "не вийшло: %s\n", up.error().c_str());
        return 1;
    }
    const Release r = up.release();
    printf("є оновлення: %s %s (%s), %lld байт\n", channel_label(r.channel).c_str(),
           r.version.c_str(), kind_label(r.kind).c_str(), (long long)r.size);
    printf("  файл: %s\n", r.url.c_str());
    printf("  sha256: %s\n", r.sha256.c_str());
    printf("  підпис перевірено\n");
    if (!fetch) return 0;

    printf("качаю…\n");
    fflush(stdout);
    up.download();
    while (up.state() == Updater::State::Downloading) usleep(200000);
    if (up.state() != Updater::State::Ready) {
        fprintf(stderr, "не завантажилося: %s\n", up.error().c_str());
        return 1;
    }
    printf("завантажено, сума збіглася\n");
    if (!put) return 0;

    // І власне підміна. Після неї execv замінює процес, тож рядків нижче
    // не буде — хіба що щось не вдалося.
    const std::string bad = up.install();
    fprintf(stderr, "не встановилося: %s\n", bad.c_str());
    return 1;
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
    win.grab_hotkey();
    win.show();
    trace("самостійний режим: %s", net.status().c_str());

    ChromeBL chrome;
    chrome.set_fonts(&fonts);

    GuiWindowSDL gui;
    SettingsState sstate;
    GuiWindowSDL css_win;
    CssEditState cstate;
    cstate.text = cfg.custom_css;
    Updater updater;
    bool update_asked = false;
    const int64_t started = now_ms();

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
            if (ev.hotkey) {
                // Замок із клавіатури. Вікно чату фокус не бере, тож іншого
                // способу дотягтися до нього, коли воно клік-крізь, немає.
                look.locked = !look.locked;
                cfg.look = look;
                cfg.save();
                changed = true;
            }
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
            // Місце під смужку лишаємо лише тоді, коли вона там справді буде.
            const bool bar_now = !look.locked && (cfg.header || chrome.hovered());
            feed.set_top_pad(bar_now ? (int)ChromeBL::bar_height() + 2 : 0);
            chrome.draw_backdrop(&ctx, w, h, look);
            feed.draw(&ctx, w, h, t);
            const float was_zoom = look.zoom;
            const ChromeEvents ce =
                chrome.draw_controls(&ctx, w, h, &look, cfg.header, feed.size() == 0);
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
            const UpdateView upd = update_view(updater);
            GameView game;                  // чат усередині гри під Linux немає
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
            if (sev.css_editor) {
                if (!css_win.created() &&
                    !css_win.create("Hominka — свій CSS", 1080, 700, /*resizable=*/true,
                                    /*mono=*/true))
                    trace("редактор теми НЕ створився: %s", SDL_GetError());
                css_win.show_beside(win.x(), win.y(), win.width(), win.height());
            }
            if (sev.check_update) updater.check(cfg.channel, HOMINKA_VERSION, "");
            if (sev.start_download) updater.download();
            if (sev.do_install) {
                const std::string bad = updater.install();
                if (bad.empty()) {
                    trace("оновлення: перезапускаюся");
                    return 0;            // execv нас уже замінив, сюди не дійде
                }
                trace("оновлення не встановилося: %s", bad.c_str());
            }
            if (sev.changed) cfg.save();
        }

        // Перевірка оновлень раз на запуск і не одразу: спершу хай
        // під'єднається чат — саме заради нього програму й відкрили.
        if (cfg.auto_update && !update_asked && now_ms() - started > 5000) {
            update_asked = true;
            updater.check(cfg.channel, HOMINKA_VERSION, "");
        }

        // Редактор теми. Правка лягає просто в стрічку — саме тому окремого
        // «попереднього перегляду» тут немає: людина бачить не схожу картинку,
        // а точно те, що побачить глядач.
        if (css_win.begin()) {
            const CssEditEvents cev = draw_css_editor(&cstate, css_win.width(),
                                                      css_win.height(), t);
            css_win.end();
            css_win.drag(cev.title_active);
            if (cev.close) css_win.hide();
            if (cev.reset) {
                cstate.text.clear();
                cfg.custom_css.clear();
                feed.set_css("");
                cfg.save();
                changed = true;
            }
            if (cev.apply) {
                cfg.custom_css = cstate.text;
                feed.set_css(cfg.custom_css);
                cfg.save();
                changed = true;
            }
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
            "  (без ключів)                         сама програма\n"
            "  --selftest <вхід.json> <вихід.png>   намалювати зразки\n"
            "  --nettest <площадка> <канал> [сек]   прочитати живий чат\n"
            "  --updatecheck [канал] [версія] [--download|--install]\n"
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
    if (!strcmp(args[0], "--updatecheck")) {
        const char* ch = args.size() > 1 && args[1][0] != '-' ? args[1] : "stable";
        const char* pretend = "";
        bool fetch = false, put = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (!strcmp(args[i], "--download")) fetch = true;
            else if (!strcmp(args[i], "--install")) { fetch = true; put = true; }
            else if (args[i][0] != '-' && args[i] != ch) pretend = args[i];
        }
        return update_check(ch, pretend, fetch, put);
    }

    if (!strcmp(args[0], "--probe")) return probe();
    if (!strcmp(args[0], "--nettest")) {
        if (args.size() < 3) { usage(); return 1; }
        return nettest(args[1], args[2], args.size() > 3 ? atoi(args[3]) : 20);
    }
    if (!strcmp(args[0], "--selftest")) {
        if (args.size() < 3) { usage(); return 1; }
        return selftest(args[1], args[2]);
    }
    usage();
    return 1;
}
