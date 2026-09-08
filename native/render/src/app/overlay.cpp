#include "app/overlay.h"

#include <psapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "app/ipc_mode.h"
#include "app/offscreen.h"
#include "app/runtime.h"
#include "common/dcomp_window.h"
#include "core/chat_doc.h"
#include "core/config.h"
#include "core/feed.h"
#include "core/version.h"
#include "gfx/imgcache.h"
#include "net/chatnet.h"
#include "net/imgfetch.h"
#include "net/viewers.h"
#include "platform/frame_writer.h"
#include "platform/gamewin.h"
#include "platform/ipc.h"
#include "ui/chrome.h"
#include "ui/cssedit_ui.h"
#include "ui/gui_win.h"
#include "ui/samples.h"
#include "ui/settings_ui.h"
#include "update/updater.h"

namespace hominka {

namespace {

// --- самостійний режим ------------------------------------------------------
//
// «Самостійний» означає рівно одне: Python не потрібен. Канали, картинки й
// налаштування програма веде сама, а вікно налаштувань — це друге вікно того
// самого процесу. Старий режим (--run <pid>) поки лишається поруч: доки все не
// перевірено в бою, ламати робочий шлях зарано.

// Картинки, яких ще немає, — у чергу качання. Значки й емоути приходять
// адресами, а не байтами, тож поки їх немає, рядок показує свій запасний
// текст — і перемальовується сам, щойно картинка приїде.
void want_images(const ChatMessage& m, ImageCache* images, ImageFetch* fetch) {
    for (const auto& e : m.emotes)
        if (!e.url.empty() && !images->known(e.url)) fetch->want(e.url);
    for (const auto& b : m.badge_icons)
        if (!b.url.empty() && !images->known(b.url)) fetch->want(b.url);
}

// Події з площадок → стрічка.
bool pump_chat(ChatNet* net, Feed* feed, ImageCache* images, ImageFetch* fetch, int64_t t) {
    bool changed = false;
    ChatEvent ev;
    // Не більше жмені за кадр: на бурхливому каналі суцільний потік інакше
    // з'їв би кадр цілком, і чат перестав би малюватися взагалі.
    for (int i = 0; i < 32 && net->take(&ev); ++i) {
        switch (ev.type) {
        case ChatEvent::Type::Message:
            want_images(ev.msg, images, fetch);
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
// спільний із розкладкою, і робити його потокобезпечним заради кількох
// емоутів на секунду означало б платити блокуванням у найгарячішому місці.
// Стан оновлювача → те, що бачить людина. Складаємо тут, бо панель про
// Windows нічого не знає й знати не повинна.
UpdateView update_view(const Updater& up) {
    UpdateView v;
    v.supported = true;
    const Release rel = up.release();

    // Опис випуску показуємо там, де є що описувати: коли оновлення пропонують
    // або вже завантажили. У решті станів це просто чужа новина.
    const bool describe = up.state() == Updater::State::Available ||
                          up.state() == Updater::State::Downloading ||
                          up.state() == Updater::State::Ready;
    if (describe && !rel.version.empty()) {
        v.title = channel_label(rel.channel) + " " + rel.version + " · " +
                  kind_label(rel.kind);
        v.notes = rel.notes;
        v.warning = rel.warning;
        if (rel.size > 0) {
            char buf[32];
            snprintf(buf, sizeof buf, "%.0f МБ", (double)rel.size / 1e6);
            v.size = buf;
        }
        v.mandatory = rel.mandatory;
    }

    switch (up.state()) {
    case Updater::State::Idle:
        v.status = "Версія " HOMINKA_VERSION ".";
        v.can_check = true;
        break;
    case Updater::State::Checking:
        v.status = "Питаю сервер оновлень…";
        break;
    case Updater::State::UpToDate:
        v.status = "У вас найсвіжіша версія (" HOMINKA_VERSION ").";
        v.can_check = true;
        break;
    case Updater::State::Available:
        v.status = "Є що поставити.";
        v.available = true;
        v.can_download = true;
        v.can_check = true;
        break;
    case Updater::State::Downloading:
        v.status = "Завантажую " + rel.version + "…";
        v.percent = up.percent();
        break;
    case Updater::State::Ready:
        v.status = "Завантажено й перевірено. Програма закриється й відкриється вже "
                   "оновленою.";
        v.can_install = true;
        break;
    case Updater::State::Failed:
        v.status = "Не вийшло: " + up.error();
        v.can_check = true;
        break;
    }
    return v;
}

// Те, що панель показує в розділі «чат поверх гри». Список вікон тримаємо тут,
// бо перелічувати їх щокадру ні до чого: він міняється, коли людина запускає
// гру, а не шістдесят разів на секунду.
struct GameState {
    std::vector<GameWindow> windows;
    int picked = 0;
    std::string status;
    bool injected = false;
    unsigned injected_pid = 0;
};

GameView game_view(const GameState& gs) {
    GameView v;
    v.supported = true;
    v.injector = injector_available();
    for (const GameWindow& w : gs.windows) {
        std::string title = w.title.size() > 40 ? w.title.substr(0, 40) + "…" : w.title;
        v.windows.push_back(title + " — " + w.exe);
    }
    v.picked = gs.picked;
    v.status = gs.status;
    v.injected = gs.injected;
    v.restorable = borderless_window() != nullptr;
    if (!gs.windows.empty() && gs.picked < (int)gs.windows.size())
        v.fso_off = fso_disabled(game_exe_path(gs.windows[(size_t)gs.picked].hwnd));
    return v;
}

// Зразки повідомлень у стрічці, доки відкритий редактор теми.
//
// Три речі, яких бракувало першому підходу: вони сипалися всі разом (тему
// підбирають на русі стрічки, а не на готовій купі), кінчалися (а дивитися
// треба довше, ніж сім рядків) і лишалися в чаті після закриття редактора,
// хоч ніхто їх не писав.
struct Demo {
    bool on = false;
    size_t next = 0;
    unsigned seq = 0;
    int64_t last = 0;
    std::vector<std::string> ids;

    void clear(Feed* feed) {
        if (!ids.empty()) rlog("зразки: прибираю %u", (unsigned)ids.size());
        for (const std::string& id : ids) feed->remove_id(id);
        ids.clear();
        next = 0;
    }

    // Які площадки показувати. Зразки мають бути схожі на СВІЙ чат: у того, хто
    // веде лише Twitch, рядки з Kick і YouTube нічого не пояснюють про його
    // тему — вони просто інші.
    bool wants(const std::string& platform, const Config& cfg) const {
        const bool any = !cfg.twitch.empty() || !cfg.kick.empty() || !cfg.youtube.empty();
        if (!any) return true;                       // каналів ще немає — показуємо всі
        if (platform == "twitch") return !cfg.twitch.empty();
        if (platform == "kick") return !cfg.kick.empty();
        if (platform == "youtube") return !cfg.youtube.empty();
        return true;                                 // свій сайт і системні події
    }

    // Один рядок раз на 900 мс, по колу й без кінця: тему підбирають на
    // стрічці, яка рухається, а не на застиглій купі. Дійшли кінця списку —
    // починаємо його спочатку, а найдавніші свої рядки прибираємо, щоб стрічка
    // не росла нескінченно.
    bool tick(Feed* feed, ImageCache* images, ImageFetch* fetch, const Config& cfg,
              int64_t t) {
        if (!on || t - last < 900) return false;
        last = t;
        const std::vector<ChatMessage> all = demo_messages();
        if (all.empty()) return false;

        // Пропускаємо чужі площадки. Обмежуємо кількість спроб розміром списку:
        // якщо не підійшов жоден, краще нічого не додати, ніж крутитися вічно.
        size_t skipped = 0;
        while (skipped < all.size()) {
            if (next >= all.size()) next = 0;
            if (wants(all[next].platform, cfg)) break;
            ++next;
            ++skipped;
        }
        if (skipped >= all.size()) return false;
        if (next >= all.size()) next = 0;

        ChatMessage m = all[next++];
        char id[32];
        snprintf(id, sizeof id, "demo-%u", (unsigned)seq++);
        m.id = id;
        want_images(m, images, fetch);
        feed->add(m, t);
        ids.push_back(m.id);
        while (ids.size() > 30) {
            feed->remove_id(ids.front());
            ids.erase(ids.begin());
        }
        return true;
    }
};

// Налаштування «анімовані емоути» → режим кеша картинок.
Motion motion_of(const std::string& name) {
    if (name == "freeze") return Motion::Freeze;
    if (name == "hide") return Motion::Hide;
    return Motion::Play;
}
// Глядачі одним рядком для смужки вікна. Порожньо — коли показувати нічого:
// або вимкнено в налаштуваннях, або жодна площадка ще не відповіла.
std::string viewers_line(const Viewers& v, const Config& cfg) {
    if (!cfg.viewers_show) return "";
    const Viewers::Count tw = cfg.viewers_twitch ? v.twitch() : Viewers::Count();
    const Viewers::Count kk = cfg.viewers_kick ? v.kick() : Viewers::Count();
    const Viewers::Count yt = cfg.viewers_youtube ? v.youtube() : Viewers::Count();

    if (cfg.viewers_sum) {
        int n = 0;
        bool known = false;
        for (const Viewers::Count* c : {&tw, &kk, &yt})
            if (c->known) { known = true; n += c->n; }
        return known ? group_digits(n) : std::string();
    }
    // Окремо — з літерою площадки: без неї три числа поспіль ні про що.
    std::string out;
    const std::pair<const char*, const Viewers::Count*> parts[] = {
        {"T", &tw}, {"K", &kk}, {"Y", &yt}};
    for (const auto& p : parts) {
        if (!p.second->known) continue;
        if (!out.empty()) out += " · ";
        out += std::string(p.first) + " " + group_digits(p.second->n);
    }
    return out;
}

// Стан джерел очима панелі. Просто перекладаємо — панель не має знати ні про
// ChatNet, ні про те, скільки там потоків.
// Рядок «Зараз» для розділу «Про програму». Памʼять беремо в системи: саме це
// число й було приводом до всього переносу, і показати його чесно доречно.
std::string about_facts(const ChatNet& net, const DCompWindow& win) {
    PROCESS_MEMORY_COUNTERS pmc = {};
    pmc.cb = sizeof pmc;
    const unsigned mb = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)
                            ? (unsigned)(pmc.WorkingSetSize / (1024 * 1024))
                            : 0;
    int live = 0;
    for (const ChatNet::SourceInfo& s : net.sources())
        if (s.connected) ++live;

    char buf[160];
    snprintf(buf, sizeof buf, "%u МБ памʼяті · вікно %d×%d · читаємо джерел: %d",
             mb, win.width(), win.height(), live);
    return buf;
}

std::vector<SourceView> source_view(const ChatNet& net, const Viewers& viewers) {
    std::vector<SourceView> out;
    for (const ChatNet::SourceInfo& s : net.sources()) {
        SourceView v;
        v.name = s.name;
        v.configured = s.configured;
        v.connected = s.connected;
        v.note = s.note;

        const std::string n = v.name;
        const Viewers::Count c = n == "Twitch"    ? viewers.twitch()
                                 : n == "Kick"    ? viewers.kick()
                                 : n == "YouTube" ? viewers.youtube()
                                                  : Viewers::Count();
        v.viewers_known = c.known;
        if (c.known) v.viewers = group_digits(c.n);
        out.push_back(v);
    }
    return out;
}

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


}  // namespace

int run_overlay(DWORD parent_pid, bool standalone) {
    rlog("старт, батько pid=%lu", (unsigned long)parent_pid);

    // Стежимо за Hominka й виходимо, коли вона зникла (навіть якщо впала):
    // щоб оверлей ніколи не лишався сиротою на екрані.
    HANDLE parent = parent_pid ? OpenProcess(SYNCHRONIZE, FALSE, parent_pid) : nullptr;

    IDWriteFactory* dwrite = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&dwrite))) {
        rlog("DirectWrite недоступний — вихід");
        return 3;
    }

    DCompWindow win;
    if (!win.create(GetModuleHandleW(nullptr), L"HominkaRenderOverlay",
                    L"Hominka chat overlay")) {
        rlog("вікно не створилося — вихід");
        return 1;
    }
    if (!win.init_gfx()) {
        rlog("init_gfx не вдався — вихід");
        return 2;
    }

    ImageCache images;
    Feed feed(dwrite, &images);
    // Викинули картинку — стрічка має забути її кадрові текстури, інакше
    // тримала б посилання на те, чого вже немає.
    images.set_evict_hook([&feed](const std::string& u) { feed.forget_image(u); });
    int64_t last_gc = 0;

    // Рамка вікна. Обробник миші ставимо ДО показу вікна: інакше перші рухи
    // курсора повз ImGui, і перше натискання «не рахується».
    Chrome chrome;
    DCompWindow::set_msg_hook(&Chrome::msg_hook);
    if (!chrome.init(win.hwnd(), win.d3d(), win.d3d_ctx())) {
        rlog("ImGui не піднявся — вихід");
        return 5;
    }
    // Перші секунди рамку видно без наведення: інакше запущена програма — це
    // темний прямокутник, який нічим себе не виказує, і людина возить мишею по
    // екрану, доки випадково на нього не натрапить.
    if (standalone) chrome.begin_intro(7000);
    Look look;

    IpcServer ipc;
    if (!standalone) {
        if (!ipc.start(parent_pid)) {
            rlog("канал не створився — вихід");
            return 4;
        }
        rlog("готово, чекаю на %s", ipc.name().c_str());
    }

    // Самостійний режим: налаштування, канали й картинки — наші.
    Config cfg;
    ChatNet net;
    ImageFetch fetch;
    GuiWindow gui;
    SettingsState sstate;
    GuiWindow css_win;
    CssEditState cstate;
    Demo demo;
    Updater updater;
    Viewers viewers;
    bool update_asked = false;
    GameState games;
    if (standalone) {
        cfg.load();
        look = cfg.look;
        feed.set_zoom(look.zoom);
        feed.set_css(cfg.custom_css);
        if (!cfg.layout.empty()) feed.set_layout(cfg.layout);
        images.set_motion(motion_of(cfg.motion));
        fetch.start();
        net.apply(cfg);
        viewers.configure(cfg.twitch, cfg.kick, cfg.youtube);
        cstate.text = cfg.custom_css;
        // Архіви минулих оновлень — двісті мегабайтів кожен, а %TEMP% Windows
        // сама не чистить.
        Updater::cleanup_downloads();
        games.windows = list_windows();
        win.move_to(cfg.x, cfg.y);
        // Замок вимикають і з клавіатури: вікно чату фокусу не бере, тож
        // єдиний спосіб — глобальне сполучення. Те саме, що було в Python.
        RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT, VK_SPACE);
        rlog("самостійний режим: %s", net.status().c_str());
    }

    // Перевірка вигляду: обидва вікна приховані від захоплення екрана, тож
    // знімок робимо самі — і виходимо. Той самий спосіб, що й «shot» у
    // робочому режимі.
    const char* shot_prefix = standalone ? getenv("HOMINKA_UI_SHOT") : nullptr;
    // Скільки чекати перед знімками. За замовчуванням досить, щоб під'єднатися;
    // жвавому чату дають більше — інакше в кадрі порожня стрічка.
    const char* shot_wait_env = getenv("HOMINKA_UI_WAIT");
    const int64_t shot_wait = shot_wait_env ? (int64_t)atoi(shot_wait_env) * 1000 : 9500;
    const int64_t started = now_ms();
    // Панель знімаємо по розділах: побачити треба кожен, а не лише той, що
    // відкрився першим.
    int settings_shots = 0;
    bool shot_chat = false;
    // Редактор знімаємо двічі: зі списком проблем і з довідником. Довідник —
    // це кілька сотень рядків згенерованого тексту, і подивитися на нього
    // очима варто хоча б раз.
    int css_shots = 0;

    int want_w = 430, want_h = 560;
    if (standalone) { want_w = cfg.w; want_h = cfg.h; feed.set_width(want_w); }
    // blanked у самостійному режимі починається з «так»: тоді перший же кадр
    // намалюється, і вікно видно ще до першого повідомлення. Інакше свіжо
    // встановлена програма не показала б узагалі нічого — і не було б на що
    // навести, щоб дістатися налаштувань.
    bool enabled = true, bye = false, logged_first = false, blanked = standalone;
    std::string shot_path;
    // Розмір вікна веде Python, АЛЕ поки його тягнуть за куточок — веде рука.
    // Інакше кожен «config» смикав би вікно назад під час розтягування.
    bool user_sizing = false;
    bool chrome_was_visible = false;
    bool force_frame = false;
    InjectState inject;
    FrameWriter writer;
    bool inject_was_on = false;
    std::vector<uint8_t> frame_px;
    unsigned tick = 0;
    std::vector<IpcFrame> frames;

    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                rlog("WM_QUIT");
                // Шар Vulkan лишати зареєстрованим після виходу ні до чого: він
                // вантажився б у чужі Vulkan-програми, яким до нас байдуже.
                if (standalone) vklayer_unregister();
                chrome.shutdown();
                return 0;
            }
            if (standalone && msg.message == WM_HOTKEY) {
                look.locked = !look.locked;
                cfg.look.locked = look.locked;
                cfg.save();
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) {
            rlog("Hominka зникла — виходжу");
            chrome.shutdown();
            return 0;
        }

        bool changed = false;
        if (standalone) {
            changed |= pump_chat(&net, &feed, &images, &fetch, now_ms());
            changed |= pump_images(&fetch, &images, &feed);
            changed |= demo.tick(&feed, &images, &fetch, cfg, now_ms());
        } else {
            frames.clear();
            ipc.poll(&frames);
            for (const auto& fr : frames)
                changed |= apply_frame(fr, &feed, &images, &look, &inject, &want_w, &want_h,
                                       &enabled, &bye, &shot_path);
        }
        if (bye) {
            rlog("Hominka попросила завершитися");
            chrome.shutdown();
            return 0;
        }

        // Курсор над нашим же вікном налаштувань чи редактора — це не
        // наведення на чат, навіть якщо він під ними. Інакше смужка чату
        // блимала щоразу, коли миша йшла до налаштувань повз край вікна.
        bool over_our_gui = false;
        {
            POINT cur;
            RECT r;
            if (GetCursorPos(&cur)) {
                if (gui.visible() && GetWindowRect(gui.hwnd(), &r) && PtInRect(&r, cur))
                    over_our_gui = true;
                if (css_win.visible() && GetWindowRect(css_win.hwnd(), &r) &&
                    PtInRect(&r, cur))
                    over_our_gui = true;
            }
        }
        chrome.poll_hover(win.hwnd(), over_our_gui);

        // Порожня стрічка — показувати нічого. Але в самостійному режимі вікно
        // чату це єдиний шлях до налаштувань: не малювати його зовсім означало
        // б показати свіжо встановлену програму порожнім екраном без жодної
        // кнопки.
        if (!enabled || (feed.size() == 0 && !standalone)) {
            // Показувати нічого. Чистимо ОДИН раз і далі GPU не чіпаємо: саме
            // безумовний Present щокадру колись відбирав відеокарту в гри.
            if (win.shown() && !blanked) { win.present_transparent(); blanked = true; }
            Sleep(16);
            continue;
        }

        const int64_t t = now_ms();
        // Розмір: поки вікно тягнуть за куточок, головний тут — курсор, і
        // «config» від Python його не перебиває.
        RECT wr = win.screen_rect();
        if (user_sizing) {
            want_w = wr.right - wr.left;
            want_h = wr.bottom - wr.top;
            feed.set_width(want_w);
        }
        const bool resized = (want_w != win.width() || want_h != win.height());

        // Рамку показуємо, лише коли миша над вікном і воно не замкнене. Поки
        // її не видно — і перемальовувати нема чого, тож кадр не рухається.
        // «Малювати рамку» і «є що перемальовувати» — не одне й те саме: у
        // перші секунди замкнене вікно кнопок не показує, але напис «я тут»
        // показує, і той напис теж треба колись стерти.
        const bool chrome_visible = chrome.visible(look.locked) || chrome.intro_active();
        const bool chrome_dirty = chrome_visible || chrome_was_visible;

        // force_frame — «намалюй іще раз, навіть якщо здається, що нічого не
        // змінилося». Потрібне рівно там, де рядки ЗНИКАЮТЬ: стрічка після
        // цього спокійна, малювати їй нема чого, і на екрані так і лишалися б
        // пікселі попереднього кадру — прибрані зразки виглядали б як
        // неприбрані.
        if (changed || resized || blanked || chrome_dirty || force_frame || feed.dirty(t)) {
            force_frame = false;
            if (!win.ensure_size(want_w, want_h)) {
                rlog("ensure_size %dx%d не вдався", want_w, want_h);
                Sleep(100);
                continue;
            }
            // Прозорість стосується всього вікна, а не самої лише підкладки:
            // крізь напівпрозорий чат має бути видно те, що під ним. А смужка
            // керування з'являється поверх стрічки, тож поки вона може
            // з'явитися, стрічка не займає верхні пікселі; замкнене вікно
            // смужки не показує — і місце їй не потрібне.
            feed.set_alpha(look.opacity);
            feed.set_top_pad(look.locked ? 0 : 30);
            win.begin_draw();
            win.d2d()->Clear(D2D1::ColorF(0, 0, 0, 0));
            // Підкладка й рамка — під чатом; сам чат — поверх.
            chrome.draw_backdrop(win.d2d(), win.width(), win.height(), look);
            feed.draw(win.d2d(), win.width(), win.height(), t);
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
            const HRESULT hr = win.end_draw();
            if (FAILED(hr)) rlog("EndDraw не вдався hr=0x%08lX", (unsigned long)hr);

            // Тепер той самий задній буфер бере D3D11 — і ImGui малює керування
            // поверх усього.
            ID3D11RenderTargetView* rtv = win.rtv();
            if (rtv) win.d3d_ctx()->OMSetRenderTargets(1, &rtv, nullptr);
            const ChromeEvents cev =
                chrome.draw_controls(win.width(), win.height(), &look, win.hwnd(),
                                     standalone ? viewers_line(viewers, cfg) : std::string(),
                                     /*can_close=*/standalone, feed.size() == 0);
            // Поки тягнуть — розмір веде рука; відпустили (geometry_changed) —
            // знову веде Python.
            if (chrome.wants_mouse()) user_sizing = true;
            if (standalone) {
                // Правда про налаштування тепер тут, а не в Python: те, що
                // покрутили в рамці, лягає в той самий config.json.
                if (cev.look_changed || cev.lock_changed) {
                    feed.set_zoom(look.zoom);
                    cfg.look = look;
                    cfg.save();
                }
                if (cev.geometry_changed) user_sizing = false;
                // Хрестик у смужці. Досі закрити програму можна було лише
                // через диспетчер задач: вікно чату фокусу не бере, тож ані
                // Alt+F4, ані системного меню в нього немає.
                if (cev.close) {
                    rlog("закрито з рамки");
                    vklayer_unregister();
                    chrome.shutdown();
                    return 0;
                }
                if (cev.open_settings) {
                    if (!gui.created() &&
                        !gui.create(L"HominkaSettings", L"Hominka — налаштування", 720, 520))
                        rlog("вікно налаштувань не створилося (лишаємося без нього)");
                    gui.show_beside(win.screen_rect());
                }
            } else {
                report_chrome(&ipc, cev, look, win, &user_sizing, &feed);
            }

            // Кадр для оверлея, вкладеного в гру. Читання з відеокарти
            // коштує грошей, тому робимо його ЛИШЕ коли інжект увімкнено — і
            // лише коли кадр справді перемальовано (ми вже в цій гілці).
            if (inject.on) {
                if (writer.ready() || writer.open()) {
                    if (win.capture(&frame_px)) {
                        float fx, fy, fw, fh;
                        win.monitor_fraction(&fx, &fy, &fw, &fh);
                        writer.write(frame_px.data(), win.width(), win.height(),
                                     fx, fy, fw, fh, inject.opacity, inject.pid,
                                     inject.hide_obs);
                    }
                } else if (!inject_was_on && writer.conflict()) {
                    rlog("кадр у грі вже пише інший продюсер — не втручаюсь");
                }
            }

            if (!shot_path.empty()) {
                // Саме тут, ДО present: у flip-моделі після показу задній буфер
                // уже інший, і знімок дістав би не той кадр. І саме з буфера, а
                // не перемальовуванням: рамку малює D3D11, обхідний шлях її б не
                // побачив.
                const int n = MultiByteToWideChar(CP_UTF8, 0, shot_path.c_str(), -1,
                                                  nullptr, 0);
                std::wstring wp((size_t)(n > 0 ? n : 1), L'\0');
                if (n > 0)
                    MultiByteToWideChar(CP_UTF8, 0, shot_path.c_str(), -1, &wp[0], n);
                const bool ok = dump_window_png(&win, wp.c_str());
                rlog("знімок %s: %s", ok ? "збережено" : "НЕ вдався", shot_path.c_str());
                shot_path.clear();
            }

            // Знімок вікна чату для перевірки вигляду — ТУТ, до показу.
            //
            // Той самий підводний камінь, що й вище: у flip-моделі після
            // Present задній буфер уже інший, і знімок, зроблений пізніше по
            // ходу циклу, показував кадр, який був ДО останньої зміни. На
            // стрічці, яка щойно завмерла, це виглядало як «прибрані зразки не
            // прибралися» — і двічі відправило шукати неіснуючу помилку.
            if (shot_prefix && !shot_chat && now_ms() - started > shot_wait - 1500) {
                shot_chat = true;
                wchar_t path[512];
                _snwprintf(path, 512, L"%hs-chat.png", shot_prefix);
                rlog("знімок чату: %d", (int)dump_window_png(&win, path));
            }

            win.present();
            blanked = false;
            win.show();
            chrome_was_visible = chrome_visible;
            if (!logged_first) {
                logged_first = true;
                rlog("перший кадр %dx%d, повідомлень %d", win.width(), win.height(),
                     (int)feed.size());
            }
        }

        // Замок: поки миша не над вікном або воно замкнене — клік-крізь, і
        // вікно не заважає ані грі, ані робочому столу. Щойно курсор зайшов —
        // пропускаємо натискання, щоб рамкою можна було скористатися.
        win.set_click_through(look.locked || !(chrome.hovered() || chrome.wants_mouse()));

        // Інжект вимкнули — кажемо DLL, що чату більше немає, і відпускаємо
        // спільну памʼять. Інакше в грі так і висів би останній кадр.
        if (inject_was_on && !inject.on) {
            writer.disable();
            writer.close();
            rlog("кадр у грі вимкнено");
        } else if (!inject_was_on && inject.on) {
            rlog("кадр у грі увімкнено (pid=%lu, opacity=%lu, hide_obs=%d)",
                 (unsigned long)inject.pid, (unsigned long)inject.opacity,
                 (int)inject.hide_obs);
        }
        inject_was_on = inject.on;

        // Панель налаштувань — друге вікно того самого процесу, тож і кадр її
        // малюється тут же, після чату.
        if (standalone) {
            if (gui.begin()) {
                const SettingsEvents sev =
                    draw_settings(&sstate, &cfg, source_view(net, viewers),
                                  update_view(updater),
                                  game_view(games), about_facts(net, win),
                                  gui.width(), gui.height());
                // Знімок — ДО показу: у flip-моделі після Present задній буфер
                // уже інший, і в PNG потрапила б порожнеча.
                // Знімаємо не одразу після показу: панель просить у системи
                // свою висоту, і застосується це лише наступним кадром.
                const bool want_shot =
                    shot_prefix && settings_shots < 5 && now_ms() - started > shot_wait;
                gui.end(!want_shot);
                if (want_shot) {
                    wchar_t path[512];
                    _snwprintf(path, 512, L"%hs-settings%d.png", shot_prefix,
                               settings_shots + 1);
                    rlog("знімок налаштувань %d: %d", settings_shots + 1,
                         (int)dump_gui_png(&gui, path));
                    gui.present();
                    if (settings_shots == 0) {
                        _snwprintf(path, 512, L"%hs-chrome.png", shot_prefix);
                        rlog("знімок рамки: %d", (int)dump_window_png(&win, path));
                    }
                    ++settings_shots;
                    sstate.page = settings_shots;      // наступний розділ
                }
                gui.drag(sev.title_active);
                if (sev.close) gui.hide();
                if (sev.look_changed) {
                    look.opacity = cfg.look.opacity;
                    look.bg_alpha = cfg.look.bg_alpha;
                    look.zoom = cfg.look.zoom;
                    look.frameless = cfg.look.frameless;
                    feed.set_zoom(look.zoom);
                }
                if (sev.sources_changed) {
                    net.apply(cfg);
                    viewers.configure(cfg.twitch, cfg.kick, cfg.youtube);
                }
                if (sev.motion_changed) {
                    images.set_motion(motion_of(cfg.motion));
                    // Повернення руху чистить кеш — картинки треба попросити
                    // знову, інакше на їх місці лишиться код емоута до першого
                    // нового повідомлення.
                    fetch.forget();
                    for (const std::string& u : feed.animated_in_use()) fetch.want(u);
                }
                if (sev.css_editor) {
                    // Редактор теми — окреме вікно, і його можна тягнути за
                    // краї: код і довідник поруч у вузькому не вміщаються.
                    if (!css_win.created() &&
                        !css_win.create(L"HominkaCssEditor", L"Hominka — свій CSS", 1080,
                                        700, /*resizable=*/true, /*mono=*/true))
                        rlog("вікно редактора теми не створилося");
                    css_win.show_beside(win.screen_rect());
                }
                if (sev.pick_game >= 0) {
                    games.picked = sev.pick_game;
                    games.status.clear();
                }
                if (sev.refresh_games) {
                    games.windows = list_windows();
                    games.picked = 0;
                    games.status.clear();
                }
                GameWindow* target =
                    games.picked < (int)games.windows.size() && !games.windows.empty()
                        ? &games.windows[(size_t)games.picked]
                        : nullptr;
                if (sev.toggle_fso && target) {
                    const std::string exe = game_exe_path(target->hwnd);
                    const bool off = fso_disabled(exe);
                    games.status = set_fso_disabled(exe, !off)
                                       ? (off ? "Повноекранну оптимізацію повернено як було."
                                              : "Готово. Перезапустіть гру — і чат буде видно "
                                                "в бою.")
                                       : "Не вдалося змінити налаштування гри.";
                }
                if (sev.make_borderless && target)
                    games.status = make_borderless(target->hwnd)
                                       ? "Готово: вікно гри тепер безрамкове."
                                       : "Не вдалося змінити це вікно (уже безрамкове або "
                                         "зникло).";
                if (sev.restore_window)
                    games.status = restore_window(borderless_window())
                                       ? "Повернули вікну гри те, що в нього було."
                                       : "Нема чого повертати.";
                if (sev.inject && target) {
                    // Шар Vulkan реєструємо ПЕРЕД вкладенням: Vulkan-гру не
                    // можна доповнити після старту, тож шар має бути на місці
                    // ще до її наступного запуску.
                    vklayer_register();
                    const InjectResult r = inject_into(target->hwnd);
                    games.status = r.message;
                    games.injected = r.ok;
                    games.injected_pid = r.pid;
                    inject.on = r.ok;
                    inject.pid = r.pid;
                }
                if (sev.stop_inject) {
                    games.injected = false;
                    inject.on = false;
                    vklayer_unregister();
                    games.status = "Чат у грі вимкнено.";
                }
                inject.opacity = (uint32_t)cfg.game_opacity;
                inject.hide_obs = cfg.game_hide_obs;

                if (sev.check_update) updater.check(cfg.channel, HOMINKA_VERSION, "");
                if (sev.start_download) updater.download();
                if (sev.do_install) {
                    const std::string bad = updater.install();
                    if (bad.empty()) {
                        rlog("оновлення: підмінник запущено, виходжу");
                        cfg.flush(true);
                        chrome.shutdown();
                        return 0;
                    }
                    rlog("оновлення не встановилося: %s", bad.c_str());
                }
                if (sev.changed) cfg.save();
            }

            // Редактор теми. Правка лягає прямо в стрічку — саме тому окремого
            // «попереднього перегляду» тут і немає: людина бачить не схожу
            // картинку, а точно те, що побачить глядач.
            if (css_win.begin()) {
                const CssEditEvents cev2 =
                    draw_css_editor(&cstate, css_win.width(), css_win.height(), now_ms());
                const bool want_shot =
                    shot_prefix && css_shots < 2 && now_ms() - started > shot_wait;
                css_win.end(!want_shot);
                if (want_shot) {
                    wchar_t path[512];
                    _snwprintf(path, 512, L"%hs-css%d.png", shot_prefix, css_shots + 1);
                    rlog("знімок редактора %d: %d", css_shots + 1,
                         (int)dump_gui_png(&css_win, path));
                    css_win.present();
                    cstate.tab = 2;               // наступний — довідник
                    ++css_shots;
                }
                css_win.drag(cev2.title_active);
                if (cev2.close) {
                    // Зразки живуть рівно доти, доки відкритий редактор: інакше
                    // людина закриває вікно, а в чаті лишаються чужі
                    // повідомлення, яких вона не писала.
                    demo.on = false;
                    cstate.samples_on = false;
                    demo.clear(&feed);
                    force_frame = true;
                    css_win.hide();
                }
                if (cev2.reset) {
                    // Скидання — це порожній свій CSS: далі діє наше типове
                    // оформлення, те саме, що бачить людина до першої правки.
                    cstate.text.clear();
                    cfg.custom_css.clear();
                    feed.set_css("");
                    cfg.save();
                    force_frame = true;
                    rlog("свій CSS скинуто до типового");
                }
                if (cev2.apply) {
                    cfg.custom_css = cstate.text;
                    feed.set_css(cfg.custom_css);
                    cfg.save();
                }
                if (cev2.samples) {
                    demo.on = !demo.on;
                    cstate.samples_on = demo.on;
                    if (!demo.on) {
                        demo.clear(&feed);
                        force_frame = true;
                    } else {
                        demo.next = 0;
                        demo.last = 0;
                    }
                }
            }
            // Вікно посунули або розтягнули — запам'ятовуємо, де воно тепер.
            const RECT r = win.screen_rect();
            if (r.left != cfg.x || r.top != cfg.y ||
                (int)(r.right - r.left) != cfg.w || (int)(r.bottom - r.top) != cfg.h) {
                cfg.x = r.left;
                cfg.y = r.top;
                cfg.w = r.right - r.left;
                cfg.h = r.bottom - r.top;
                cfg.save();
            }
            cfg.flush();

            // Перевірка оновлень раз на запуск і не одразу: спершу хай
            // під'єднається чат — саме заради нього програму й відкрили.
            if (cfg.auto_update && !update_asked && now_ms() - started > 5000) {
                update_asked = true;
                updater.check(cfg.channel, HOMINKA_VERSION, "");
            }

            // Вікна ще не показані — показуємо: знімок робиться саме з них.
            if (shot_prefix && now_ms() - started > shot_wait - 1500) {
                // Сам чат уже знято вище, до Present. Тепер наводимо курсор:
                // рамку видно, лише коли на вікно наведено, і перевіряти її
                // вигляд інакше нічим.
                {
                    const RECT r = win.screen_rect();
                    SetCursorPos((r.left + r.right) / 2, r.top + 10);
                }
                if (!gui.visible()) {
                    if (!gui.created())
                        gui.create(L"HominkaSettings", L"Hominka — налаштування", 720, 520);
                    gui.show_beside(win.screen_rect());
                }
                if (!css_win.visible()) {
                    if (!css_win.created())
                        css_win.create(L"HominkaCssEditor", L"Hominka — свій CSS", 1080,
                                       700, true, true);
                    css_win.show_beside(win.screen_rect());
                }
            }
            if (shot_prefix && settings_shots >= 5 && css_shots >= 2) {
                if (games.injected) vklayer_unregister();
                chrome.shutdown();
                return 0;
            }
        }

        // Гра в повноекранному сидить у вищому z-band — тримаємось зверху, але
        // не щокадру: раз на ~250 мс досить. У рідкісних старих іграх це дає
        // мерехтіння — на цей випадок є перемикач у налаштуваннях.
        if ((tick++ % 16) == 0 && (!standalone || cfg.keep_top)) win.keep_topmost();
        Sleep(16);
    }
}

}  // namespace hominka
