#include "badges.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <set>

#include "net_http.h"

namespace hominka {

namespace {

using json = nlohmann::json;

const int64_t TTL_GLOBAL_MS = 6 * 60 * 60 * 1000;
const int64_t TTL_CHANNEL_MS = 30 * 60 * 1000;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Вбудовані іконки значків Kick — ті самі, що на сервері.
// Стилізовані впізнавані гліфи, не власна графіка Kick.
const std::map<std::string, std::string> kKickSvg = {
    {"broadcaster",
     "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nIzUzZmMxOCcvPjxwYXRoIGQ9J00xMiA5LjVhMi41IDIuNSAwIDEgMCAwIDUgMi41IDIuNSAwIDAgMCAwLTV6bS01LjYtMy4xIDEuNCAxLjRhNS42IDUuNiAwIDAgMCAwIDguNGwtMS40IDEuNGE3LjYgNy42IDAgMCAxIDAtMTEuMnptMTEuMiAwYTcuNiA3LjYgMCAwIDEgMCAxMS4ybC0xLjQtMS40YTUuNiA1LjYgMCAwIDAgMC04LjR6TTQuNiA0IDYgNS40YTkuNiA5LjYgMCAwIDAgMCAxMy4yTDQuNiAyMGExMS42IDExLjYgMCAwIDEgMC0xNnptMTQuOCAwYTExLjYgMTEuNiAwIDAgMSAwIDE2TDE4IDE4LjZhOS42IDkuNiAwIDAgMCAwLTEzLjJ6JyBmaWxsPScjMDgyMTBhJy8+PC9zdmc+"},
    {"mod",
     "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nIzAwYzJhOCcvPjxwYXRoIGQ9J00xMiAyIDQgNXY2YzAgNSAzLjQgOC4zIDggOS42IDQuNi0xLjMgOC00LjYgOC05LjZWNXonIGZpbGw9JyNmZmYnLz48L3N2Zz4="},
    {"vip",
     "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nI2MwNGJmZicvPjxwYXRoIGQ9J00zIDcuNSA3IDExbDUtNiA1IDYgNC0zLjUtMS44IDEwLjVINC44eicgZmlsbD0nI2ZmZicvPjwvc3ZnPg=="},
    {"sub",
     "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nI2ZmYjMxZicvPjxwYXRoIGQ9J00xMiAzLjVsMi42IDUuMyA1LjkuOS00LjMgNC4xIDEgNS44LTUuMi0yLjctNS4yIDIuNyAxLTUuOC00LjMtNC4xIDUuOS0uOXonIGZpbGw9JyMzYTI0MDAnLz48L3N2Zz4="},
    {"verified",
     "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nIzFlOWJmMCcvPjxwYXRoIGQ9J00xMCAxNS40IDYuNCAxMS44IDUgMTMuMiAxMCAxOC4yIDE5IDkuMmwtMS40LTEuNHonIGZpbGw9JyNmZmYnLz48L3N2Zz4="},
    {"staff",
     "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nIzhhOGY5OCcvPjxwYXRoIGQ9J00xMiA4LjVhMy41IDMuNSAwIDEgMCAwIDcgMy41IDMuNSAwIDAgMCAwLTd6bTktLjUtMi0uNmE3IDcgMCAwIDAtLjYtMS40bDEtMS44LTEuNC0xLjQtMS44IDFhNyA3IDAgMCAwLTEuNC0uNkwxNCAxaC0ybC0uNiAyYTcgNyAwIDAgMC0xLjQuNmwtMS44LTFMNi44IDRsMSAxLjhhNyA3IDAgMCAwLS42IDEuNEw1IDh2MmwyIC42YTcgNyAwIDAgMCAuNiAxLjRsLTEgMS44IDEuNCAxLjQgMS44LTFhNyA3IDAgMCAwIDEuNC42TDEyIDE5aDJsLjYtMmE3IDcgMCAwIDAgMS40LS42bDEuOCAxIDEuNC0xLjQtMS0xLjhhNyA3IDAgMCAwIC42LTEuNGwyLS42eicgZmlsbD0nI2ZmZicvPjwvc3ZnPg=="},
    {"og",
     "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nI2ZmN2EwMCcvPjxwYXRoIGQ9J00xMS45NSAxMS45N0MxMS45NSAxMi44NiAxMS43NiAxMy42NiAxMS4zNyAxNC4zN0MxMC45OSAxNS4wNyAxMC40NSAxNS42MiA5Ljc2IDE2LjAxQzkuMDcgMTYuNDAgOC4zMCAxNi42MCA3LjQ0IDE2LjYwQzYuNjEgMTYuNjAgNS44NSAxNi40MSA1LjE3IDE2LjAzQzQuNDggMTUuNjUgMy45NSAxNS4xMiAzLjU3IDE0LjQ0QzMuMTkgMTMuNzUgMy4wMCAxMi45OCAzLjAwIDEyLjExQzMuMDAgMTEuMjEgMy4xOSAxMC40MCAzLjU4IDkuNjdDMy45NiA4Ljk1IDQuNTAgOC4zOSA1LjE5IDguMDBDNS44OSA3LjYwIDYuNjggNy40MCA3LjU2IDcuNDBDOC40MSA3LjQwIDkuMTcgNy41OSA5Ljg1IDcuOTdDMTAuNTIgOC4zNSAxMS4wNCA4Ljg5IDExLjQwIDkuNTlDMTEuNzcgMTAuMjkgMTEuOTUgMTEuMDggMTEuOTUgMTEuOTdNOS4xMSAxMi4wNUM5LjExIDExLjMyIDguOTcgMTAuNzQgOC42OSAxMC4zMkM4LjQxIDkuOTAgOC4wMiA5LjY4IDcuNTQgOS42OEM3LjAwIDkuNjggNi41OSA5Ljg5IDYuMjkgMTAuMjlDNS45OSAxMC43MCA1Ljg1IDExLjI3IDUuODUgMTIuMDFDNS44NSAxMi43MyA1Ljk5IDEzLjMwIDYuMjkgMTMuNzBDNi41OCAxNC4xMSA2Ljk5IDE0LjMxIDcuNDkgMTQuMzFDNy44MCAxNC4zMSA4LjA4IDE0LjIyIDguMzMgMTQuMDRDOC41OCAxMy44NSA4Ljc3IDEzLjU5IDguOTEgMTMuMjZDOS4wNCAxMi45MiA5LjExIDEyLjUxIDkuMTEgMTIuMDVNMjEuMDAgMTUuODZDMjAuNjcgMTYuMDUgMjAuMTcgMTYuMjIgMTkuNTAgMTYuMzdDMTguODMgMTYuNTIgMTguMjAgMTYuNjAgMTcuNjAgMTYuNjBDMTYuMDQgMTYuNjAgMTQuODQgMTYuMjEgMTMuOTkgMTUuNDNDMTMuMTQgMTQuNjUgMTIuNzIgMTMuNTYgMTIuNzIgMTIuMTVDMTIuNzIgMTEuMjEgMTIuOTMgMTAuMzcgMTMuMzYgOS42NEMxMy43OSA4LjkxIDE0LjM5IDguMzUgMTUuMTcgNy45N0MxNS45NCA3LjU5IDE2Ljg2IDcuNDAgMTcuOTEgNy40MEMxOC42OSA3LjQwIDE5LjUxIDcuNTEgMjAuMzkgNy43M0wyMC4zOSAxMC4wN0MxOS45OCA5Ljg4IDE5LjU4IDkuNzUgMTkuMjEgOS42OEMxOC44MyA5LjYxIDE4LjQyIDkuNTcgMTcuOTggOS41N0MxNy4yNCA5LjU3IDE2LjY1IDkuNzkgMTYuMjIgMTAuMjRDMTUuNzggMTAuNjggMTUuNTYgMTEuMjkgMTUuNTYgMTIuMDZDMTUuNTYgMTIuODAgMTUuNzUgMTMuMzggMTYuMTEgMTMuNzlDMTYuNDggMTQuMjEgMTYuOTkgMTQuNDIgMTcuNjUgMTQuNDJDMTcuOTUgMTQuNDIgMTguMTggMTQuNDAgMTguMzIgMTQuMzdMMTguMzIgMTMuMTBMMTYuNjIgMTMuMTBMMTYuNjIgMTEuMDdMMjEuMDAgMTEuMDdMMjEuMDAgMTUuODYnIGZpbGw9JyNmZmYnIGZpbGwtcnVsZT0nZXZlbm9kZCcvPjwvc3ZnPg=="},
};

// Публічний ідентифікатор веб-клієнта Twitch — той самий, що йде зі сторінки
// twitch.tv у кожному запиті. Не секрет і не ключ доступу: без нього GraphQL
// просто не відповідає.
const char* kGqlClientId = "kimne78kx3ncx6brgo4mv6wki5h1ko";
const char* kGqlUrl = "https://gql.twitch.tv/gql";

// Список значків → «набір/версія» → URL картинки.
void collect(const json& list, std::map<std::string, std::string>* out) {
    if (!list.is_array()) return;
    for (const auto& b : list) {
        if (!b.is_object()) continue;
        auto sid = b.find("setID");
        auto ver = b.find("version");
        auto img = b.find("imageURL");
        if (sid == b.end() || ver == b.end() || img == b.end()) continue;
        if (!sid->is_string() || !ver->is_string() || !img->is_string()) continue;
        const std::string url = img->get<std::string>();
        if (!url.empty())
            (*out)[sid->get<std::string>() + "/" + ver->get<std::string>()] = url;
    }
}

// Значки Twitch тепер віддає лише GraphQL.
//
// Був відкритий badges.twitch.tv/v1/badges/…, і саме його читала стара версія
// програми. Його більше немає — ім'я навіть не розв'язується, тож значки
// мовчки зникали. GraphQL відповідає без жодного входу, треба лише той самий
// ідентифікатор клієнта, що й у сторінці.
json gql(const std::string& query) {
    json body;
    body["query"] = query;
    const HttpResult r = http_post(kGqlUrl, body.dump(), {{"Client-ID", kGqlClientId}}, 20);
    if (!r.ok()) return json::object();
    try {
        const json d = json::parse(r.body);
        auto data = d.find("data");
        return data == d.end() ? json::object() : *data;
    } catch (const std::exception&) {
        return json::object();
    }
}

void fetch_global(std::map<std::string, std::string>* out) {
    const json d = gql("query { badges { setID version imageURL(size: DOUBLE) } }");
    auto it = d.find("badges");
    if (it != d.end()) collect(*it, out);
}

// Канальні значки — за числовим id каналу: рівно те, що дає тег room-id.
void fetch_channel(const std::string& room_id, std::map<std::string, std::string>* out) {
    const json d = gql("query { user(id: \"" + room_id +
                       "\") { broadcastBadges { setID version imageURL(size: DOUBLE) } } }");
    auto u = d.find("user");
    if (u == d.end() || !u->is_object()) return;
    auto it = u->find("broadcastBadges");
    if (it != u->end()) collect(*it, out);
}

}  // namespace

std::string twitch_badge_name(const std::string& set_id) {
    // Той самий перелік, що й у текстових плашках: інакше та сама людина
    // отримала б картинку одного значка й підпис іншого.
    static const std::map<std::string, std::string> kNorm = {
        {"broadcaster", "broadcaster"}, {"moderator", "mod"}, {"vip", "vip"},
        {"subscriber", "sub"}, {"founder", "sub"}, {"partner", "verified"},
        {"staff", "staff"}, {"admin", "staff"}, {"global_mod", "staff"},
        {"artist-badge", "artist"},
    };
    auto it = kNorm.find(set_id);
    return it == kNorm.end() ? std::string() : it->second;
}

const std::map<std::string, std::string>& Badges::global() {
    const int64_t t = now_ms();
    if (global_.by_key.empty() || t - global_.fetched_ms > TTL_GLOBAL_MS) {
        std::map<std::string, std::string> fresh;
        fetch_global(&fresh);
        // Порожня відповідь не витирає те, що вже маємо: краще вчорашній набір,
        // ніж чат без значків.
        if (!fresh.empty()) global_.by_key.swap(fresh);
        global_.fetched_ms = t;
    }
    return global_.by_key;
}

const std::map<std::string, std::string>& Badges::channel(const std::string& room_id) {
    Set& e = channels_[room_id];
    const int64_t t = now_ms();
    if (e.by_key.empty() || t - e.fetched_ms > TTL_CHANNEL_MS) {
        std::map<std::string, std::string> fresh;
        fetch_channel(room_id, &fresh);
        if (!fresh.empty()) e.by_key.swap(fresh);
        e.fetched_ms = t;
    }
    return e.by_key;
}

std::vector<BadgeIcon> Badges::twitch(const std::string& room_id, const std::string& tag) {
    std::vector<BadgeIcon> out;
    if (tag.empty()) return out;

    std::lock_guard<std::mutex> lock(mx_);
    const auto& g = global();
    const std::map<std::string, std::string>* ch =
        room_id.empty() ? nullptr : &channel(room_id);

    std::set<std::string> seen;
    size_t pos = 0;
    while (pos <= tag.size()) {
        size_t end = tag.find(',', pos);
        if (end == std::string::npos) end = tag.size();
        const std::string part = tag.substr(pos, end - pos);
        pos = end + 1;
        if (part.empty()) continue;

        const size_t slash = part.find('/');
        const std::string set_id = part.substr(0, slash);
        const std::string ver = slash == std::string::npos ? "" : part.substr(slash + 1);
        const std::string nid = twitch_badge_name(set_id);
        if (nid.empty() || seen.count(nid)) continue;

        const std::string key = set_id + "/" + ver;
        const std::string* url = nullptr;
        if (ch) {
            auto it = ch->find(key);           // канал перебиває глобальний набір
            if (it != ch->end()) url = &it->second;
        }
        if (!url) {
            auto it = g.find(key);
            if (it != g.end()) url = &it->second;
        }
        if (!url || url->empty()) continue;
        seen.insert(nid);
        out.push_back({nid, *url});
    }
    return out;
}

std::vector<BadgeIcon> Badges::kick(const std::vector<std::string>& norm) {
    std::vector<BadgeIcon> out;
    for (const std::string& nid : norm) {
        auto it = kKickSvg.find(nid);
        if (it != kKickSvg.end()) out.push_back({nid, it->second});
    }
    return out;
}

void Badges::warm(const std::string& room_id) {
    std::lock_guard<std::mutex> lock(mx_);
    global();
    if (!room_id.empty()) channel(room_id);
}

Badges& badges() {
    static Badges b;
    return b;
}

}  // namespace hominka
