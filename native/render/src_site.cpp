#include "src_site.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "net_http.h"

namespace hominka {

namespace {

using json = nlohmann::json;

// Сайт стоїть за Cloudflare, а той відмовляє рукостисканню, яке «не схоже на
// браузер»: без звичайного User-Agent і без Origin сокет просто не
// відкривається.
const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36";

std::string str_of(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return "";
}

bool flag_of(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

// Значки автора приходять готовим списком імен — тим самим набором, що й у
// решті програми, тож перекладати нічого не треба, досить відсіяти чуже.
// Прапорці admin/mod старіші за список badges і трапляються окремо.
std::vector<std::string> badges_of(const json& d) {
    std::vector<std::string> out;
    if (flag_of(d, "admin")) out.push_back("broadcaster");
    if (flag_of(d, "mod")) out.push_back("mod");
    auto b = d.find("badges");
    if (b != d.end() && b->is_array())
        for (const auto& one : *b) {
            if (!one.is_string()) continue;
            const std::string name = one.get<std::string>();
            bool have = false;
            for (const auto& n : out) if (n == name) { have = true; break; }
            if (!have) out.push_back(name);
        }
    return out;
}

std::vector<BadgeIcon> icons_of(const json& d) {
    std::vector<BadgeIcon> out;
    auto it = d.find("badgeIcons");
    if (it == d.end() || !it->is_array()) return out;
    for (const auto& one : *it) {
        if (!one.is_object()) continue;
        const std::string id = str_of(one, "id"), url = str_of(one, "url");
        if (!id.empty() && !url.empty()) out.push_back({id, url});
    }
    return out;
}

std::vector<Emote> emotes_of(const json& d) {
    std::vector<Emote> out;
    auto it = d.find("emotes");
    if (it == d.end() || !it->is_array()) return out;
    for (const auto& one : *it) {
        if (!one.is_object()) continue;
        const std::string code = str_of(one, "code"), url = str_of(one, "url");
        if (!code.empty() && !url.empty()) out.push_back({code, url});
    }
    return out;
}

// Звідки повідомлення прийшло насправді. Сервер мостить чужі чати у свій і в
// кожному пише, чиє воно: значок площадки біля рядка має відповідати правді.
std::string platform_of(const std::string& source) {
    const std::string src = trimmed(source);
    if (src.empty() || src == "site" || src == "donation") return "site";
    if (src == "twitch" || src == "kick" || src == "youtube") return src;
    return "site";
}

}  // namespace

std::string site_ws_url(const std::string& page_url) {
    std::string raw = trimmed(page_url);
    if (raw.empty()) return "";
    if (raw.find("://") == std::string::npos) raw = "https://" + raw;

    const size_t scheme_end = raw.find("://");
    const std::string scheme = raw.substr(0, scheme_end);
    const std::string rest = raw.substr(scheme_end + 3);

    const size_t cut = rest.find_first_of("/?#");
    const std::string host = cut == std::string::npos ? rest : rest.substr(0, cut);
    if (host.empty()) return "";
    const std::string tail = cut == std::string::npos ? "" : rest.substr(cut);

    const size_t q = tail.find('?');
    const std::string path = q == std::string::npos ? tail : tail.substr(0, q);
    const std::string query = q == std::string::npos ? "" : tail.substr(q);

    const bool secure = scheme == "https" || scheme == "wss";
    const std::string base = (secure ? "wss://" : "ws://") + host;

    // Сокетів на сервері два, і який із них потрібен — видно зі сторінки.
    //
    // «/chat…» — публічний бік для глядачів: сокет поруч, за /ws, і ключів там
    // не треба. «/overlay/…» — особисте вікно стримера, і його сокет інший
    // (/ws/overlay), а ключ із адреси сторінки саме той, що пускає всередину,
    // тож параметри переносимо як є.
    //
    // Плутати їх не можна: публічний /ws на ключ від оверлея відповідає
    // «400 Bad Request», і виглядає це як «сайт не працює».
    if (path.compare(0, 9, "/overlay/") == 0 || path == "/overlay")
        return base + "/ws/overlay" + query;
    return base + "/ws";
}

SiteSource::SiteSource() = default;
SiteSource::~SiteSource() { stop(); }

bool SiteSource::start(const std::string& page_url, const std::set<std::string>& skip,
                       ChatSink sink) {
    const std::string url = site_ws_url(page_url);
    if (url.empty()) { error_ = "не схоже на посилання"; return false; }
    skip_ = skip;
    sink_ = std::move(sink);

    ix::initNetSystem();
    ws_.reset(new ix::WebSocket());
    ws_->setUrl(url);
    ws_->setPingInterval(30);
    ix::SocketTLSOptions tls;
    tls.caFile = ca_bundle_path();
    ws_->setTLSOptions(tls);

    ix::WebSocketHttpHeaders headers;
    headers["User-Agent"] = kUserAgent;
    // Origin потрібен тому ж Cloudflare: сокет без нього виглядає як не з
    // браузера, і рукостискання відхиляється.
    {
        const bool secure = url.compare(0, 6, "wss://") == 0;
        const size_t beg = url.find("//") + 2;
        const size_t end = url.find('/', beg);
        const std::string host =
            url.substr(beg, end == std::string::npos ? end : end - beg);
        headers["Origin"] = (secure ? "https://" : "http://") + host;
    }
    ws_->setExtraHeaders(headers);

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) { connected_ = true; return; }
        if (msg->type == ix::WebSocketMessageType::Close) { connected_ = false; return; }
        if (msg->type == ix::WebSocketMessageType::Error) {
            error_ = msg->errorInfo.reason;
            connected_ = false;
            return;
        }
        if (msg->type == ix::WebSocketMessageType::Message) on_text(msg->str);
    });
    ws_->start();
    return true;
}

void SiteSource::on_text(const std::string& raw) {
    json d;
    try {
        d = json::parse(raw);
    } catch (const std::exception&) {
        return;
    }
    if (!d.is_object() || !sink_) return;
    const std::string type = str_of(d, "type");

    if (type == "delete") {
        // Сервер видаляє за числовим id — приводимо до того ж вигляду, що й у
        // наших повідомленнях.
        ChatEvent ev;
        ev.type = ChatEvent::Type::Delete;
        ev.id = str_of(d, "deleteId");
        if (!ev.id.empty()) sink_(ev);
        return;
    }
    if (type == "system") {
        const std::string text = trimmed(str_of(d, "text"));
        if (text.empty()) return;
        ChatEvent ev;
        ev.msg.platform = "site";
        ev.msg.kind = "system";
        ev.msg.text = text;
        sink_(ev);
        return;
    }
    if (type == "donation") {
        // Донат — це теж повідомлення, просто з сумою.
        std::string amount = trimmed(str_of(d, "amount"));
        const std::string cur = trimmed(str_of(d, "currency"));
        if (!amount.empty() && !cur.empty()) amount += " " + cur;
        else if (amount.empty()) amount = cur;

        std::string nick = str_of(d, "nick");
        if (nick.empty()) nick = str_of(d, "displayName");
        if (nick.empty()) nick = "Анонім";

        ChatEvent ev;
        ev.msg.platform = "site";
        ev.msg.id = str_of(d, "id");
        ev.msg.nick = nick;
        ev.msg.name = str_of(d, "displayName").empty() ? nick : str_of(d, "displayName");
        ev.msg.text = str_of(d, "text");
        ev.msg.amount = amount;
        ev.msg.emotes = emotes_of(d);
        sink_(ev);
        return;
    }
    if (type != "chat") return;
    // history / stats / title / users / reaction — це для сторінки чату, а не
    // для стрічки: ані списку глядачів, ані назви ефіру в ній немає.

    // Прибране автомодерацією доходить до нас лише в особистому вікні
    // стримера; показувати його в чаті поверх гри ні до чого.
    if (flag_of(d, "deleted")) return;

    const std::string platform = platform_of(str_of(d, "source"));
    if (platform != "site" && skip_.count(platform)) return;

    const std::string nick = str_of(d, "nick");
    const std::string text = str_of(d, "text");
    if (text.empty()) return;

    ChatEvent ev;
    ev.msg.platform = platform;
    ev.msg.id = str_of(d, "id");
    ev.msg.nick = nick;
    ev.msg.name = str_of(d, "displayName").empty() ? nick : str_of(d, "displayName");
    ev.msg.text = text;
    ev.msg.color = str_of(d, "color");
    ev.msg.reply = str_of(d, "replyTo");
    ev.msg.badges = badges_of(d);
    ev.msg.badge_icons = icons_of(d);
    ev.msg.emotes = emotes_of(d);
    sink_(ev);
}

void SiteSource::stop() {
    if (ws_) {
        ws_->stop();
        ws_.reset();
    }
    connected_ = false;
}

}  // namespace hominka
