#include "app/ipc_mode.h"

#include <nlohmann/json.hpp>

#include "app/runtime.h"
#include "core/chat_doc.h"

using json = nlohmann::json;

namespace hominka {

namespace {

// Рядок із кадру: чого немає — того й немає, і це нормально.
std::string get_str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

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

}  // namespace hominka
