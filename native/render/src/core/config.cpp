#include "core/config.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "core/chat_doc.h"

namespace hominka {

namespace {

using json = nlohmann::json;

// Скільки чекати перед справжнім записом. Вікно тягнуть плавно, а це десятки
// подій на секунду; писати файл на кожну — марно тривожити диск.
const int64_t SAVE_DELAY_MS = 400;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void make_dir(const std::string& path) {
#ifdef _WIN32
    CreateDirectoryA(path.c_str(), nullptr);
#else
    mkdir(path.c_str(), 0755);
#endif
}

float num(const json& j, const char* key, float def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<float>() : def;
}

int inum(const json& j, const char* key, int def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<int>() : def;
}

bool flag(const json& j, const char* key, bool def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

}  // namespace

std::string data_dir() {
    std::string dir;
#ifdef _WIN32
    // %LOCALAPPDATA%\Hominka — звичне місце для даних застосунку, і воно НЕ
    // приховане: шукати налаштування в прихованій теці поруч із .exe —
    // знущання. Оновлення програми цю теку не чіпає.
    const char* base = getenv("LOCALAPPDATA");
    if (base && *base) dir = std::string(base) + "\\Hominka";
    else {
        const char* home = getenv("USERPROFILE");
        dir = std::string(home ? home : ".") + "\\Hominka";
    }
#else
    // Поза Windows «~/Hominka» виглядало б чужорідно: домовленість інша.
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) dir = std::string(xdg) + "/hominka";
    else {
        const char* home = getenv("HOME");
        dir = std::string(home ? home : ".") + "/.config/hominka";
    }
#endif
    make_dir(dir);
    return dir;
}

void Config::load() {
#ifdef _WIN32
    path_ = data_dir() + "\\config.json";
#else
    path_ = data_dir() + "/config.json";
#endif

    std::ifstream f(path_.c_str(), std::ios::binary);
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // BOM: варто відкрити config.json у «Блокноті» й зберегти — Windows допише
    // три байти на початку, і розбір спіткнеться об них, мовчки скинувши всі
    // налаштування на типові.
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);

    try {
        raw_ = json::parse(text);
    } catch (const std::exception&) {
        raw_ = json::object();
        return;
    }
    if (!raw_.is_object()) { raw_ = json::object(); return; }

    const json& g = raw_.value("geometry", json::object());
    if (g.is_object() && g.contains("x") && g.contains("y") &&
        g.contains("w") && g.contains("h")) {
        x = inum(g, "x", x);
        y = inum(g, "y", y);
        w = inum(g, "w", w);
        h = inum(g, "h", h);
    }
    look.opacity = num(raw_, "opacity", look.opacity);
    look.bg_alpha = num(raw_, "bg_alpha", look.bg_alpha);
    look.zoom = zoom_clamp(num(raw_, "zoom", look.zoom));
    look.frameless = flag(raw_, "frameless", look.frameless);
    keep_top = flag(raw_, "keepTop", keep_top);
    header = flag(raw_, "header", header);

    const json& lk = raw_.value("lock", json::object());
    if (lk.is_object()) {
        look.lock_bg = flag(lk, "bg", look.lock_bg);
        look.lock_viewers = flag(lk, "viewers", look.lock_viewers);
        look.lock_header = flag(lk, "header", look.lock_header);
    }

    youtube = str(raw_, "myChannel");
    twitch = str(raw_, "twitchChannel");
    kick = str(raw_, "kickChannel");
    site_url = str(raw_, "siteChatUrl");
    custom_css = str(raw_, "customCss");
    chat_delay = inum(raw_, "chatDelay", chat_delay);
    const std::string m = str(raw_, "animatedEmotes");
    if (m == "play" || m == "freeze" || m == "hide") motion = m;

    auto lay = raw_.find("chatLayout");
    if (lay != raw_.end() && lay->is_array()) {
        std::vector<std::string> l;
        for (const auto& s : *lay) if (s.is_string()) l.push_back(s.get<std::string>());
        layout = clean_layout(l);
    }

    const json& vw = raw_.value("viewers", json::object());
    if (vw.is_object()) {
        viewers_show = flag(vw, "show", viewers_show);
        viewers_twitch = flag(vw, "twitch", viewers_twitch);
        viewers_kick = flag(vw, "kick", viewers_kick);
        viewers_youtube = flag(vw, "youtube", viewers_youtube);
        viewers_sum = flag(vw, "sum", viewers_sum);
    }

    const json& go = raw_.value("gameOverlay", json::object());
    if (go.is_object()) {
        game_opacity = inum(go, "opacity", game_opacity);
        game_hide_obs = flag(go, "hideFromObs", game_hide_obs);
    }

    const std::string ch = str(raw_, "channel");
    if (!ch.empty()) channel = ch;
    auto_update = flag(raw_, "autoUpdate", auto_update);
}

void Config::save() {
    if (!dirty_) dirty_since_ = now_ms();
    dirty_ = true;
}

void Config::flush(bool force) {
    if (!dirty_) return;
    if (!force && now_ms() - dirty_since_ < SAVE_DELAY_MS) return;
    dirty_ = false;
    if (path_.empty()) return;

    // Міняємо СВОЇ ключі в тому, що прочитали, а не збираємо файл наново:
    // усе, чого ми ще не знаємо, має пережити наш запис.
    if (!raw_.is_object()) raw_ = json::object();
    raw_["geometry"] = {{"x", x}, {"y", y}, {"w", w}, {"h", h}};
    raw_["opacity"] = look.opacity;
    raw_["bg_alpha"] = look.bg_alpha;
    raw_["zoom"] = look.zoom;
    raw_["frameless"] = look.frameless;
    raw_["keepTop"] = keep_top;
    raw_["header"] = header;
    raw_["lock"] = {{"bg", look.lock_bg},
                    {"viewers", look.lock_viewers},
                    {"header", look.lock_header}};
    raw_["myChannel"] = youtube;
    raw_["twitchChannel"] = twitch;
    raw_["kickChannel"] = kick;
    raw_["siteChatUrl"] = site_url;
    raw_["customCss"] = custom_css;
    raw_["chatLayout"] = layout;
    raw_["chatDelay"] = chat_delay;
    raw_["animatedEmotes"] = motion;
    raw_["viewers"] = {{"show", viewers_show},
                       {"twitch", viewers_twitch},
                       {"kick", viewers_kick},
                       {"youtube", viewers_youtube},
                       {"sum", viewers_sum}};
    raw_["gameOverlay"] = {{"opacity", game_opacity}, {"hideFromObs", game_hide_obs}};
    raw_["channel"] = channel;
    raw_["autoUpdate"] = auto_update;

    // Пишемо через тимчасовий файл і переставляємо: обірваний на півдорозі
    // запис (падіння, вимкнення) інакше лишив би нечитабельний config.json, і
    // всі налаштування зникли б назовсім.
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << raw_.dump();
        if (!f) return;
    }
#ifdef _WIN32
    MoveFileExA(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    rename(tmp.c_str(), path_.c_str());
#endif
}

}  // namespace hominka
