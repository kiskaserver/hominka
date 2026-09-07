#include "emotes.h"

#include <chrono>
#include <nlohmann/json.hpp>

#include "net_http.h"

namespace hominka {

namespace {

using json = nlohmann::json;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string str_of(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

// --- 7TV ------------------------------------------------------------------
//
// Просимо саме .webp: він менший за png і, головне, саме в ньому приїжджає
// анімація. Розмір 2x — на звичайному кеглі чату більший не видно.
std::string seventv_url(const std::string& id) {
    return "https://cdn.7tv.app/emote/" + id + "/2x.webp";
}

void seventv_map(const json& list, std::map<std::string, std::string>* out) {
    if (!list.is_array()) return;
    for (const auto& e : list) {
        if (!e.is_object()) continue;
        const std::string name = str_of(e, "name");
        const std::string id = str_of(e, "id");
        if (!name.empty() && !id.empty()) out->emplace(name, seventv_url(id));
    }
}

// --- BetterTTV ------------------------------------------------------------
//
// Без розширення: BTTV віддає емоут у його РІДНОМУ форматі — анімовані як GIF,
// решту як PNG. Просити «.webp» означало б утратити анімацію в частині з них.
std::string bttv_url(const std::string& id) {
    return "https://cdn.betterttv.net/emote/" + id + "/2x";
}

void bttv_map(const json& list, std::map<std::string, std::string>* out) {
    if (!list.is_array()) return;
    for (const auto& e : list) {
        if (!e.is_object()) continue;
        const std::string code = str_of(e, "code");
        const std::string id = str_of(e, "id");
        if (!code.empty() && !id.empty()) out->emplace(code, bttv_url(id));
    }
}

// --- FrankerFaceZ ---------------------------------------------------------
//
// FFZ дає кілька розмірів у вигляді «1»/«2»/«4», і окремо — анімовані версії.
// Беремо 2x, а анімовану — за перевагою: заради неї емоут і додають.
std::string ffz_pick(const json& urls) {
    if (!urls.is_object()) return "";
    for (const char* size : {"2", "4", "1"}) {
        auto it = urls.find(size);
        if (it == urls.end() || !it->is_string()) continue;
        std::string u = it->get<std::string>();
        if (u.empty()) continue;
        if (u.compare(0, 2, "//") == 0) return "https:" + u;   // трапляється без схеми
        if (u.compare(0, 4, "http") == 0) return u;
    }
    return "";
}

void ffz_map(const json& list, std::map<std::string, std::string>* out) {
    if (!list.is_array()) return;
    for (const auto& e : list) {
        if (!e.is_object()) continue;
        const std::string name = str_of(e, "name");
        if (name.empty()) continue;
        std::string u;
        auto anim = e.find("animated");
        if (anim != e.end()) u = ffz_pick(*anim);
        if (u.empty()) {
            auto urls = e.find("urls");
            if (urls != e.end()) u = ffz_pick(*urls);
        }
        if (!u.empty()) out->emplace(name, u);
    }
}

json get_json(const std::string& url) {
    try {
        return json::parse(http_get_json(url));
    } catch (const std::exception&) {
        return json::object();
    }
}

// FFZ ховає емоути в «sets», де ключ — довільний номер набору.
void ffz_sets(const json& doc, std::map<std::string, std::string>* out) {
    auto sets = doc.find("sets");
    if (sets == doc.end() || !sets->is_object()) return;
    for (const auto& kv : sets->items()) {
        if (!kv.value().is_object()) continue;
        auto em = kv.value().find("emoticons");
        if (em != kv.value().end()) ffz_map(*em, out);
    }
}

}  // namespace

void Emotes::fetch_global(std::map<std::string, std::string>* out) {
    // Порядок навмисний: те, що поклали першим, лишається (emplace не
    // перезаписує). 7TV найсвіжіший і найпоширеніший, тож він і головний.
    const json sevenv = get_json("https://7tv.io/v3/emote-sets/global");
    auto it = sevenv.find("emotes");
    if (it != sevenv.end()) seventv_map(*it, out);

    bttv_map(get_json("https://api.betterttv.net/3/cached/emotes/global"), out);
    ffz_sets(get_json("https://api.frankerfacez.com/v1/set/global"), out);
}

void Emotes::fetch_channel(const std::string& platform, const std::string& cid,
                           std::map<std::string, std::string>* out) {
    if (cid.empty()) return;

    if (platform == "twitch") {
        ffz_sets(get_json("https://api.frankerfacez.com/v1/room/id/" + cid), out);
        const json b = get_json("https://api.betterttv.net/3/cached/users/twitch/" + cid);
        auto ch = b.find("channelEmotes");
        if (ch != b.end()) bttv_map(*ch, out);
        auto sh = b.find("sharedEmotes");
        if (sh != b.end()) bttv_map(*sh, out);
    } else if (platform == "youtube") {
        const json b = get_json("https://api.betterttv.net/3/cached/users/youtube/" + cid);
        auto ch = b.find("channelEmotes");
        if (ch != b.end()) bttv_map(*ch, out);
        auto sh = b.find("sharedEmotes");
        if (sh != b.end()) bttv_map(*sh, out);
    }
    // BTTV і FFZ каналів Kick не мають — там лише 7TV.

    const json s = get_json("https://7tv.io/v3/users/" + platform + "/" + cid);
    auto set = s.find("emote_set");
    if (set != s.end() && set->is_object()) {
        auto em = set->find("emotes");
        if (em != set->end()) seventv_map(*em, out);
    }
}

const std::map<std::string, std::string>& Emotes::global() {
    const int64_t t = now_ms();
    if (global_.by_code.empty() || t - global_.fetched_ms > EMOTES_TTL_GLOBAL_MS) {
        std::map<std::string, std::string> fresh;
        fetch_global(&fresh);
        // Порожня відповідь (мережі немає) не має витирати те, що вже є:
        // краще показувати вчорашній набір, ніж жодного.
        if (!fresh.empty()) global_.by_code.swap(fresh);
        global_.fetched_ms = t;
    }
    return global_.by_code;
}

const std::map<std::string, std::string>& Emotes::channel(const std::string& platform,
                                                          const std::string& cid) {
    Set& e = channels_[platform + ":" + cid];
    const int64_t t = now_ms();
    if (e.by_code.empty() || t - e.fetched_ms > EMOTES_TTL_CHANNEL_MS) {
        std::map<std::string, std::string> fresh;
        fetch_channel(platform, cid, &fresh);
        if (!fresh.empty()) e.by_code.swap(fresh);
        e.fetched_ms = t;
    }
    return e.by_code;
}

std::vector<EmoteRef> Emotes::append(const std::vector<EmoteRef>& have,
                                     const std::string& platform,
                                     const std::string& channel_id,
                                     const std::string& text) {
    std::vector<EmoteRef> out = have;
    if (text.empty()) return out;

    std::lock_guard<std::mutex> lock(mx_);
    const auto& g = global();
    const auto& c = channel_id.empty()
                        ? g                       // каналу не знаємо — лише загальні
                        : channel(platform, channel_id);

    // Ідемо по СЛОВАХ: емоут — це ціле слово, а не будь-який збіг у рядку.
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (unsigned char)text[i] <= ' ') ++i;
        size_t j = i;
        while (j < text.size() && (unsigned char)text[j] > ' ') ++j;
        if (j > i) {
            const std::string word = text.substr(i, j - i);
            bool known = false;
            for (const auto& e : out) if (e.code == word) { known = true; break; }
            if (!known) {
                // Спершу канальний набір, потім загальний: те, що додав
                // стример, має перевагу над однойменним загальним.
                const std::string* url = nullptr;
                auto ci = c.find(word);
                if (ci != c.end()) {
                    url = &ci->second;
                } else {
                    auto gi = g.find(word);
                    if (gi != g.end()) url = &gi->second;
                }
                if (url && !url->empty()) out.push_back({word, *url});
            }
        }
        i = j;
    }
    return out;
}

void Emotes::warm(const std::string& platform, const std::string& channel_id) {
    std::lock_guard<std::mutex> lock(mx_);
    global();
    if (!channel_id.empty()) channel(platform, channel_id);
}

Emotes& emotes() {
    static Emotes e;
    return e;
}

}  // namespace hominka
