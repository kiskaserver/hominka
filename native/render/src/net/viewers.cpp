#include "net/viewers.h"

#include <chrono>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "net/net_http.h"
#include "net/src_youtube.h"

namespace hominka {

namespace {

using json = nlohmann::json;

// Раз на хвилину. Лічильник глядачів на самих площадках оновлюється приблизно
// так само, тож частіше — це навантаження без нової інформації.
const int POLL_SECONDS = 60;

const char* kGqlUrl = "https://gql.twitch.tv/gql";
const char* kGqlClientId = "kimne78kx3ncx6brgo4mv6wki5h1ko";

Viewers::Count from_twitch(const std::string& channel) {
    Viewers::Count c;
    if (channel.empty()) return c;

    json body;
    body["query"] = "query { user(login: \"" + channel + "\") { stream { viewersCount } } }";
    const HttpResult r = http_post(kGqlUrl, body.dump(), {{"Client-ID", kGqlClientId}}, 15);
    if (!r.ok()) return c;
    try {
        const json d = json::parse(r.body);
        const json& v = d["data"]["user"]["stream"]["viewersCount"];
        if (v.is_number_integer()) {
            c.known = true;
            c.n = v.get<int>();
        }
    } catch (const std::exception&) {
        // Канал не в ефірі — «stream» приходить null, і звертання до нього
        // кидає. Це не помилка, це відповідь «зараз ніхто».
    }
    return c;
}

Viewers::Count from_kick(const std::string& channel) {
    Viewers::Count c;
    if (channel.empty()) return c;

    const HttpResult r = http_get("https://kick.com/api/v2/channels/" + channel, 15);
    if (!r.ok()) return c;
    try {
        const json d = json::parse(r.body);
        auto ls = d.find("livestream");
        if (ls == d.end() || !ls->is_object()) return c;   // не в ефірі
        auto n = ls->find("viewer_count");
        if (n != ls->end() && n->is_number_integer()) {
            c.known = true;
            c.n = n->get<int>();
        }
    } catch (const std::exception&) {
    }
    return c;
}

// YouTube окремого способу спитати не має, тож читаємо сторінку трансляції.
// Шукаємо «originalViewCount» саме поруч із videoViewCountRenderer: у тому ж
// документі трапляються й інші лічильники, і взяти перший-ліпший означало б
// показувати кількість переглядів замість кількості глядачів.
Viewers::Count from_youtube(const std::string& channel) {
    Viewers::Count c;
    if (channel.empty()) return c;

    const std::string html = youtube_live_page(channel);
    if (html.empty()) return c;

    const size_t at = html.find("\"videoViewCountRenderer\"");
    if (at == std::string::npos) return c;
    // Дивимося лише в межах цього блоку: далі в документі є й інші лічильники,
    // і взяти перший-ліпший означало б показати кількість переглядів замість
    // кількості глядачів.
    const size_t near_end = at + 400 < html.size() ? at + 400 : html.size();

    const size_t live = html.find("\"isLive\":true", at);
    if (live == std::string::npos || live > near_end) return c;

    const std::string key = "\"originalViewCount\":\"";
    const size_t k = html.find(key, at);
    if (k == std::string::npos || k > near_end) return c;
    const size_t beg = k + key.size();
    const size_t end = html.find('"', beg);
    if (end == std::string::npos) return c;

    const std::string digits = html.substr(beg, end - beg);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
        return c;
    c.known = true;
    c.n = atoi(digits.c_str());
    return c;
}

}  // namespace

std::string group_digits(int n) {
    std::string s = std::to_string(n < 0 ? 0 : n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, " ");
    return s;
}

Viewers::~Viewers() { stop(); }

void Viewers::configure(const std::string& twitch, const std::string& kick,
                        const std::string& youtube) {
    {
        std::lock_guard<std::mutex> lock(mx_);
        if (twitch == twitch_ch_ && kick == kick_ch_ && youtube == youtube_ch_) return;
        twitch_ch_ = twitch;
        kick_ch_ = kick;
        youtube_ch_ = youtube;
        // Канал змінився — старе число вже нічиє.
        twitch_ = kick_ = youtube_ = Count();
    }
    if (!worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(wake_mx_);
            stopping_ = false;
        }
        worker_ = std::thread(&Viewers::run, this);
        return;
    }
    // Не чекати хвилину: людина щойно вписала канал і дивиться на порожнє
    // місце, де має бути число.
    {
        std::lock_guard<std::mutex> lock(wake_mx_);
        poke_ = true;
    }
    cv_.notify_all();
}

bool Viewers::wait(int seconds) {
    std::unique_lock<std::mutex> lock(wake_mx_);
    cv_.wait_for(lock, std::chrono::seconds(seconds),
                 [this] { return stopping_ || poke_; });
    poke_ = false;
    return !stopping_;
}

void Viewers::run() {
    for (;;) {
        std::string tw, kk, yt;
        {
            std::lock_guard<std::mutex> lock(mx_);
            tw = twitch_ch_;
            kk = kick_ch_;
            yt = youtube_ch_;
        }
        const Count t = from_twitch(tw);
        const Count k = from_kick(kk);
        const Count y = from_youtube(yt);
        {
            std::lock_guard<std::mutex> lock(mx_);
            // Пишемо лише те, чий канал за цей час не змінили.
            if (tw == twitch_ch_) twitch_ = t;
            if (kk == kick_ch_) kick_ = k;
            if (yt == youtube_ch_) youtube_ = y;
        }
        if (!wait(POLL_SECONDS)) return;
    }
}

Viewers::Count Viewers::twitch() const {
    std::lock_guard<std::mutex> lock(mx_);
    return twitch_;
}

Viewers::Count Viewers::kick() const {
    std::lock_guard<std::mutex> lock(mx_);
    return kick_;
}

Viewers::Count Viewers::youtube() const {
    std::lock_guard<std::mutex> lock(mx_);
    return youtube_;
}

Viewers::Count Viewers::total() const {
    std::lock_guard<std::mutex> lock(mx_);
    Count out;
    for (const Count* c : {&twitch_, &kick_, &youtube_})
        if (c->known) {
            out.known = true;
            out.n += c->n;
        }
    return out;
}

void Viewers::stop() {
    {
        std::lock_guard<std::mutex> lock(wake_mx_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

}  // namespace hominka
