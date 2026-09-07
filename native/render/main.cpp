// Нативний рендер чату.
//
// Три режими, і малює в усіх ОДИН І ТОЙ САМИЙ код (render/feed.cpp):
//
//   --selftest вхід.json вихід.png [--width N] [--verbose] [--backdrop none]
//       Складає стрічку зі зразків і зберігає PNG. Саме цим ми звіряємося з
//       браузером: доки картинка не збігається, підключати рендер до програми
//       немає сенсу.
//
//   --run <pid> [--verbose]
//       Робочий режим: вікно DirectComposition поверх гри (у гру нічого не
//       вкладається — безпечно для античитів), повідомлення приходять від
//       Hominka іменованим каналом. <pid> — її PID: коли вона зникне, ми
//       вийдемо слідом, щоб не лишити оверлея-сироту на екрані.
//
//   --preview <pid>
//       Предпросмотр для редактора CSS. Ані вікна, ані DirectComposition: чат
//       малюється в память і йде пікселями назад у Hominka, яка показує його
//       звичайним QLabel. Окремий процес, бо предпросмотр не має нічого
//       спільного з оверлеєм на екрані — і не має права його чіпати.
//
//   --probe
//       Чи жива зв'язка litehtml + Direct2D. Потрібна після оновлення
//       litehtml або компілятора (див. native/thirdparty.sh).
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "litehtml.h"
#include "nlohmann/json.hpp"

#include "../common/dcomp_window.h"
#include "chat_doc.h"
#include "chatnet.h"
#include "chrome.h"
#include "config.h"
#include "csslint.h"
#include "cssedit_ui.h"
#include "container_d2d.h"
#include "feed.h"
#include "frame_writer.h"
#include "gamewin.h"
#include "gui_win.h"
#include "imgcache.h"
#include "imgfetch.h"
#include "ipc.h"
#include "nettest.h"
#include "page_assets.h"
#include "samples.h"
#include "settings_ui.h"
#include "updater.h"
#include "version.h"

using json = nlohmann::json;

namespace hominka {
namespace {

// Ловимо збої і пишемо зсув від початку модуля разом із ланцюжком викликів.
// Без цього окремий процес падав би без жодного сліду — а зсув лягає прямо в
// addr2line на нестрипнутій збірці й дає файл із рядком. Той самий підхід, що
// в dcomp_overlay.cpp.
LONG CALLBACK crash_veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // 0xE06D7363 — кидок C++ (bad_alloc тощо). Ловимо і його: інакше про
    // невдале виділення памʼяті ми дізнаємось лише з terminate, коли стек уже
    // розкручено й місце кидка втрачено. Друкуємо тільки перший.
    static LONG reported = 0;
    if (code == 0xC0000005 || code == 0xC0000409 || code == 0xC000001D ||
        (code == 0xE06D7363 && InterlockedExchange(&reported, 1) == 0)) {
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof name);
            fprintf(stderr, "КРАШ code=0x%lX модуль=%s зсув=0x%llX\n",
                    (unsigned long)code, name,
                    (unsigned long long)((char*)addr - (char*)mod));
        } else {
            fprintf(stderr, "КРАШ code=0x%lX addr=%p (модуль невідомий)\n",
                    (unsigned long)code, addr);
        }
        void* frames[40];
        const USHORT n = CaptureStackBackTrace(0, 40, frames, nullptr);
        fprintf(stderr, "ланцюжок викликів (%d):\n", (int)n);
        for (USHORT i = 0; i < n; ++i) {
            HMODULE m = nullptr;
            char mn[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)frames[i], &m) && m) {
                GetModuleFileNameA(m, mn, sizeof mn);
                const char* base = strrchr(mn, '\\');
                fprintf(stderr, "  [%02d] %s+0x%llX\n", (int)i, base ? base + 1 : mn,
                        (unsigned long long)((char*)frames[i] - (char*)m));
            } else {
                fprintf(stderr, "  [%02d] %p\n", (int)i, frames[i]);
            }
        }
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool g_verbose = false;
// --dump-html: вивести готову розмітку повідомлення у stdout. Потрібно, коли
// треба перевірити саме її, окремо від малювання.
bool g_dump_html = false;
// Підкладати темне тло під готовий PNG (--backdrop none вимикає).
bool g_backdrop = true;

void trace(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

// У робочому режимі консолі немає — пишемо в той самий журнал, що й решта
// оверлея (%TEMP%\hominka-overlay.log). Один файл на всі частини: коли щось
// не так, дивитися треба в одному місці, а не в трьох.
void rlog(const char* fmt, ...) {
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char path[MAX_PATH];
    const DWORD n = GetTempPathA(MAX_PATH, path);
    if (!n || n > MAX_PATH - 24) return;
    lstrcatA(path, "hominka-overlay.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] render: %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fclose(f);
}

int64_t now_ms() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (int64_t)(t.QuadPart * 1000 / freq.QuadPart);
}

// --- розбір повідомлення ----------------------------------------------------

std::string get_str(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}


// --- Direct2D для самоперевірки ---------------------------------------------

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

// --- робочий режим ----------------------------------------------------------

// Застосовує один кадр із каналу. Повертає true, якщо картинку варто
// перемалювати.
// Стан інжект-оверлея: його задає Hominka (вона й вкладає DLL у гру).
struct InjectState {
    bool on = false;
    uint32_t pid = 0;          // малювати лише в цьому процесі (0 = у будь-якому)
    uint32_t opacity = 235;
    bool hide_obs = false;
};

bool apply_frame(const IpcFrame& fr, Feed* feed, ImageCache* images, Look* look,
                 InjectState* inj, int* want_w, int* want_h, bool* enabled, bool* bye,
                 std::string* shot_path) {
    json j;
    try {
        j = json::parse(fr.json);
    } catch (const std::exception& e) {
        rlog("кадр не розібрався: %s", e.what());
        return false;
    }
    if (!j.is_object()) return false;
    const std::string t = get_str(j, "t");

    if (t == "msg")    { feed->add(message_from_json(j), now_ms()); return true; }
    if (t == "delete") { feed->remove_id(get_str(j, "id")); return true; }
    if (t == "purge")  { feed->purge_nick(get_str(j, "nick")); return true; }
    if (t == "clear")  { feed->clear(); return true; }
    if (t == "css")    { feed->set_css(get_str(j, "css")); return true; }
    if (t == "layout") {
        std::vector<std::string> l;
        auto arr = j.find("layout");
        if (arr != j.end() && arr->is_array())
            for (const auto& s : *arr) if (s.is_string()) l.push_back(s.get<std::string>());
        feed->set_layout(l);
        return true;
    }
    if (t == "config") {
        if (j.contains("zoom") && j["zoom"].is_number()) {
            look->zoom = j["zoom"].get<float>();
            feed->set_zoom(look->zoom);
        }
        if (j.contains("opacity") && j["opacity"].is_number())
            look->opacity = j["opacity"].get<float>();
        if (j.contains("bg_alpha") && j["bg_alpha"].is_number())
            look->bg_alpha = j["bg_alpha"].get<float>();
        if (j.contains("frameless") && j["frameless"].is_boolean())
            look->frameless = j["frameless"].get<bool>();
        if (j.contains("locked") && j["locked"].is_boolean())
            look->locked = j["locked"].get<bool>();
        if (j.contains("width") && j["width"].is_number()) {
            *want_w = j["width"].get<int>();
            feed->set_width(*want_w);
        }
        if (j.contains("height") && j["height"].is_number())
            *want_h = j["height"].get<int>();
        return true;
    }
    if (t == "image") {
        const std::string url = get_str(j, "url");
        if (url.empty() || fr.blob.empty()) return false;
        images->put(url, fr.blob.data(), fr.blob.size());
        // Рядки, які цю картинку чекали, перекладаємо наново — тепер вони
        // намалюють її замість запасного тексту.
        feed->on_image_arrived(url);
        // І одразу прибираємо анімовані емоути, які вже нікому не потрібні:
        // саме вони — вся вага кеша, і саме тут вона щойно зросла.
        const size_t gone = images->gc_animated(feed->animated_in_use());
        if (gone)
            rlog("кеш: викинуто %u анімованих, лишилося %u на %u КБ",
                 (unsigned)gone, (unsigned)images->animated_count(),
                 (unsigned)(images->animated_bytes() / 1024));
        return true;
    }
    if (t == "inject") {
        inj->on = j.contains("on") && j["on"].is_boolean() && j["on"].get<bool>();
        if (j.contains("pid") && j["pid"].is_number())
            inj->pid = (uint32_t)j["pid"].get<long long>();
        if (j.contains("opacity") && j["opacity"].is_number())
            inj->opacity = (uint32_t)j["opacity"].get<int>();
        if (j.contains("hide_obs") && j["hide_obs"].is_boolean())
            inj->hide_obs = j["hide_obs"].get<bool>();
        return true;
    }
    if (t == "enabled") {
        *enabled = !j.contains("on") || !j["on"].is_boolean() || j["on"].get<bool>();
        return true;
    }
    if (t == "bye") { *bye = true; return true; }
    // Знімок — це «перемалюй і збережи»: повертаємо true, інакше кадр міг би
    // не оновитися взагалі (у стрічці ж нічого не змінилося).
    if (t == "shot") { *shot_path = get_str(j, "path"); return true; }
    return false;
}

// Розповідає Hominka, що людина зробила у вікні. Сам рендер налаштувань не
// зберігає: правда про них лишається в config.json, а це лише повідомлення
// «сталося ось таке». Так не буває двох джерел істини й не треба вирішувати,
// чиє значення новіше.
void report_chrome(IpcServer* ipc, const ChromeEvents& ev, const Look& look,
                   DCompWindow& win, bool* user_sizing, Feed* feed) {
    if (ev.look_changed) {
        feed->set_zoom(look.zoom);
        json j;
        j["t"] = "look";
        j["opacity"] = look.opacity;
        j["bg_alpha"] = look.bg_alpha;
        j["zoom"] = look.zoom;
        ipc->send(j.dump());
    }
    if (ev.lock_changed) {
        json j;
        j["t"] = "lock";
        j["on"] = look.locked;
        ipc->send(j.dump());
    }
    if (ev.geometry_changed) {
        const RECT r = win.screen_rect();
        json j;
        j["t"] = "geometry";
        j["x"] = (int)r.left;
        j["y"] = (int)r.top;
        j["w"] = (int)(r.right - r.left);
        j["h"] = (int)(r.bottom - r.top);
        ipc->send(j.dump());
        *user_sizing = false;
    }
    if (ev.open_settings) {
        json j;
        j["t"] = "settings";
        ipc->send(j.dump());
    }
}

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
        v.status = channel_label(rel.channel) + " " + rel.version + " — " +
                   kind_label(rel.kind) +
                   (rel.notes.empty() ? std::string() : ".\n" + rel.notes);
        v.can_download = true;
        v.can_check = true;
        v.mandatory = rel.mandatory;
        break;
    case Updater::State::Downloading:
        v.status = "Завантажую " + rel.version + "…";
        v.percent = up.percent();
        break;
    case Updater::State::Ready:
        v.status = "Версію " + rel.version + " завантажено й перевірено. "
                   "Програма закриється й відкриється вже оновленою.";
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
    Updater updater;
    bool update_asked = false;
    GameState games;
    if (standalone) {
        cfg.load();
        look = cfg.look;
        feed.set_zoom(look.zoom);
        feed.set_css(cfg.custom_css);
        if (!cfg.layout.empty()) feed.set_layout(cfg.layout);
        fetch.start();
        net.apply(cfg);
        if (!gui.create(L"HominkaSettings", L"Hominka — налаштування", 360, 620))
            rlog("вікно налаштувань не створилося (лишаємося без нього)");
        // Редактор теми — теж окреме вікно, і його можна тягнути за краї:
        // код і довідник поруч у вузькому вікні не вміщаються.
        if (!css_win.create(L"HominkaCssEditor", L"Hominka — свій CSS", 1080, 700,
                            /*resizable=*/true, /*mono=*/true))
            rlog("вікно редактора теми не створилося");
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
    const int64_t started = now_ms();
    bool shot_settings = false;
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

        chrome.poll_hover(win.hwnd());

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
        const bool chrome_visible = chrome.hovered() && !look.locked;
        const bool chrome_dirty = chrome_visible || chrome_was_visible;

        if (changed || resized || blanked || chrome_dirty || feed.dirty(t)) {
            if (!win.ensure_size(want_w, want_h)) {
                rlog("ensure_size %dx%d не вдався", want_w, want_h);
                Sleep(100);
                continue;
            }
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
            const ChromeEvents cev = chrome.draw_controls(win.width(), win.height(),
                                                          &look, win.hwnd());
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
                if (cev.open_settings) gui.show_beside(win.screen_rect());
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
                    draw_settings(&sstate, &cfg, net.status(), update_view(updater),
                                  game_view(games), gui.width(), gui.height());
                // Знімок — ДО показу: у flip-моделі після Present задній буфер
                // уже інший, і в PNG потрапила б порожнеча.
                // Знімаємо не одразу після показу: панель просить у системи
                // свою висоту, і застосується це лише наступним кадром.
                const bool want_shot =
                    shot_prefix && !shot_settings && now_ms() - started > 9500;
                gui.end(!want_shot);
                if (want_shot) {
                    shot_settings = true;
                    wchar_t path[512];
                    _snwprintf(path, 512, L"%hs-settings.png", shot_prefix);
                    rlog("знімок налаштувань: %d", (int)dump_gui_png(&gui, path));
                    gui.present();
                    _snwprintf(path, 512, L"%hs-chat.png", shot_prefix);
                    rlog("знімок чату: %d", (int)dump_window_png(&win, path));
                }
                gui.drag(sev.title_active);
                if (sev.content_height > 0) gui.want_height(sev.content_height);
                if (sev.close) gui.hide();
                if (sev.look_changed) {
                    look.opacity = cfg.look.opacity;
                    look.bg_alpha = cfg.look.bg_alpha;
                    look.zoom = cfg.look.zoom;
                    look.frameless = cfg.look.frameless;
                    feed.set_zoom(look.zoom);
                }
                if (sev.sources_changed) net.apply(cfg);
                if (sev.css_editor) css_win.show_beside(win.screen_rect());
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
                    shot_prefix && css_shots < 2 && now_ms() - started > 9500;
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
                if (cev2.close) css_win.hide();
                if (cev2.apply) {
                    cfg.custom_css = cstate.text;
                    feed.set_css(cfg.custom_css);
                    cfg.save();
                }
                if (cev2.samples) {
                    const int64_t t0 = now_ms();
                    for (const ChatMessage& m : demo_messages()) {
                        want_images(m, &images, &fetch);
                        feed.add(m, t0);
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
            if (shot_prefix && now_ms() - started > 8000) {
                if (!gui.visible()) gui.show_beside(win.screen_rect());
                if (!css_win.visible()) css_win.show_beside(win.screen_rect());
            }
            if (shot_prefix && shot_settings && css_shots >= 2) {
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

// --- предпросмотр для редактора CSS -----------------------------------------

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

// --- самоперевірка ----------------------------------------------------------

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
    if (j.is_object()) {
        if (j.contains("css") && j["css"].is_string()) user_css = j["css"].get<std::string>();
        if (j.contains("layout") && j["layout"].is_array())
            for (const auto& s : j["layout"])
                if (s.is_string()) layout.push_back(s.get<std::string>());
        if (j.contains("zoom") && j["zoom"].is_number()) zoom = j["zoom"].get<float>();
        if (j.contains("width") && j["width"].is_number()) width = j["width"].get<int>();
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
    // анімовані емоути тут завмирають на першому кадрі.
    feed.set_animate(false);
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
    const int64_t t = FEED_ENTER_MS * 10;
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
int css_check(const wchar_t* path) {
    const std::string text = read_file(path);
    if (text.empty()) {
        fprintf(stderr, "порожній або не прочитався файл\n");
        return 2;
    }
    for (const CssProblem& p : validate_css(text))
        printf("error %d %s\n", p.line, p.text.c_str());
    for (const CssProblem& p : lint_css(text))
        printf("warn %d %s\n", p.line, p.text.c_str());
    return 0;
}

// Перевірка оновлення з командного рядка. Саме тут видно, чи сходиться підпис
// зі СПРАВЖНІМ маніфестом на сервері: формат того, що підписується, мусить
// збігатися з Python до байта, і перевірити це можна лише проти живого випуску.
int update_check(const char* channel, const char* pretend, bool fetch) {
    Updater up;
    const char* current = pretend && *pretend ? pretend : HOMINKA_VERSION;
    up.check(channel, current, "");
    for (int i = 0; i < 300 && up.state() == Updater::State::Checking; ++i) Sleep(100);

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
           r.version.c_str(), kind_label(r.kind).c_str(), r.size);
    printf("  файл: %s\n", r.url.c_str());
    printf("  sha256: %s\n", r.sha256.c_str());
    printf("  підпис перевірено\n");
    if (!fetch) return 0;

    // Завантаження — окремим кроком і лише на прохання: це двісті мегабайтів.
    // Але саме тут перевіряється те, чого інакше не побачиш: чи справді ми
    // тягнемо потоком, чи сходиться сума й чи не бреше поступ.
    printf("качаю…\n");
    fflush(stdout);
    up.download();
    int last = -1;
    while (up.state() == Updater::State::Downloading) {
        const int p = up.percent();
        if (p / 10 != last / 10) {
            last = p;
            printf("  %d%%\n", p);
            fflush(stdout);
        }
        Sleep(200);
    }
    if (up.state() != Updater::State::Ready) {
        fprintf(stderr, "не завантажилося: %s\n", up.error().c_str());
        return 1;
    }
    printf("завантажено, сума збіглася\n");
    Updater::cleanup_downloads();
    return 0;
}

// Перевірка маніфесту з файлу: чи сходиться підпис і що саме в ньому.
//
// Потрібне двічі. По-перше, це відповідь на «чому не оновлюється» — видно, чи
// річ у підписі. По-друге, саме так ганяються підміни: беремо СПРАВЖНІЙ
// маніфест, міняємо в ньому по одному полю й дивимося, що ловиться
// (release_smoke.py).
int verify_release(const wchar_t* path) {
    const std::string text = read_file(path);
    if (text.empty()) {
        fprintf(stderr, "порожній або не прочитався файл\n");
        return 2;
    }
    std::string err;
    const Release rel = parse_manifest(text, "stable", &err);
    if (!err.empty()) {
        printf("ні: %s\n", err.c_str());
        return 1;
    }
    printf("так: %s %s (%s)\n", rel.channel.c_str(), rel.version.c_str(),
           rel.kind.c_str());
    return 0;
}

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

void usage() {
    fwprintf(stderr,
             L"Використання:\n"
             L"  hominka-render-x64.exe --selftest <вхід.json> <вихід.png> [--width N]\n"
             L"  hominka-render-x64.exe --app                (сам собі програма)\n"
             L"  hominka-render-x64.exe --run <pid Hominka>\n"
             L"  hominka-render-x64.exe --preview <pid Hominka>\n"
             L"  hominka-render-x64.exe --nettest <площадка> <канал> [сек]\n"
             L"  hominka-render-x64.exe --csslint <тема.css>\n"
             L"  hominka-render-x64.exe --updatecheck [канал] [версія] [--download]\n"
             L"  hominka-render-x64.exe --verifyrelease <маніфест.json>\n"
             L"  hominka-render-x64.exe --probe\n");
}

}  // namespace
}  // namespace hominka

int wmain(int argc, wchar_t** argv) {
    AddVectoredExceptionHandler(1, hominka::crash_veh);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--verbose")) {
            hominka::g_verbose = true;
            hominka::g_draw_trace = true;
        } else if (!wcscmp(argv[i], L"--dump-html")) {
            hominka::g_dump_html = true;
        } else if (!wcscmp(argv[i], L"--backdrop") && i + 1 < argc &&
                   !wcscmp(argv[i + 1], L"none")) {
            hominka::g_backdrop = false;
        }
    }

    int rc = 1;
    if (argc >= 2 && !wcscmp(argv[1], L"--probe")) {
        hominka::g_draw_trace = true;
        rc = hominka::probe_litehtml();
    } else if (argc >= 4 && !wcscmp(argv[1], L"--nettest")) {
        // Площадка й канал із широких символів у вузькі: це завжди латиниця.
        char plat[32] = {0}, ch[128] = {0};
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, plat, sizeof plat - 1, nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, ch, sizeof ch - 1, nullptr, nullptr);
        rc = hominka::nettest(plat, ch, argc >= 5 ? _wtoi(argv[4]) : 20);
    } else if (argc >= 3 && !wcscmp(argv[1], L"--preview")) {
        rc = hominka::run_preview((DWORD)_wtoi(argv[2]));
    } else if (argc >= 3 && !wcscmp(argv[1], L"--run")) {
        rc = hominka::run_overlay((DWORD)_wtoi(argv[2]), false);
    } else if (argc >= 3 && !wcscmp(argv[1], L"--verifyrelease")) {
        rc = hominka::verify_release(argv[2]);
    } else if (argc >= 2 && !wcscmp(argv[1], L"--updatecheck")) {
        char ch[32] = "stable", pretend[32] = {0};
        bool fetch = false;
        if (argc >= 3 && argv[2][0] != L'-')
            WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, ch, sizeof ch - 1, nullptr, nullptr);
        for (int i = 3; i < argc; ++i) {
            if (!wcscmp(argv[i], L"--download")) fetch = true;
            else WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, pretend, sizeof pretend - 1,
                                     nullptr, nullptr);
        }
        rc = hominka::update_check(ch, pretend, fetch);
    } else if (argc >= 3 && !wcscmp(argv[1], L"--csslint")) {
        rc = hominka::css_check(argv[2]);
    } else if (argc >= 2 && !wcscmp(argv[1], L"--app")) {
        rc = hominka::run_overlay(0, true);
    } else if (argc >= 4 && !wcscmp(argv[1], L"--selftest")) {
        int width = 430;                       // типова ширина вікна чату
        for (int i = 4; i + 1 < argc; ++i)
            if (!wcscmp(argv[i], L"--width")) width = _wtoi(argv[i + 1]);
        rc = hominka::selftest(argv[2], argv[3], width);
    } else {
        hominka::usage();
    }

    CoUninitialize();
    return rc;
}
