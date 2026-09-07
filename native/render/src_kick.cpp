#include "src_kick.h"

#include <chrono>
#include <map>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "badges.h"
#include "emotes.h"
#include "net_http.h"

namespace hominka {

namespace {

using json = nlohmann::json;

// Публічний ключ застосунку Pusher, зашитий у сторінці Kick.
const char* kPusherUrl =
    "wss://ws-us2.pusher.com/app/32cbd69e4b950bf97679"
    "?protocol=7&client=js&version=8.4.0&flags=SSL";

const char* kEvtMessage = "App\\Events\\ChatMessageEvent";
const char* kEvtSub     = "App\\Events\\SubscriptionEvent";
const char* kEvtGift    = "App\\Events\\GiftedSubscriptionsEvent";
const char* kEvtDeleted = "App\\Events\\MessageDeletedEvent";
const char* kEvtBanned  = "App\\Events\\UserBannedEvent";
const char* kEvtPinned  = "App\\Events\\PinnedMessageCreatedEvent";
const char* kEvtReward  = "App\\Events\\RewardRedeemedEvent";
const char* kEvtHost    = "App\\Events\\StreamHostEvent";

std::string str_of(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return "";
}

// «[emote:12345:PogChamp]» у тексті → «:PogChamp:» плюс картинка.
//
// Розгортаємо саме в читабельний код, а не викидаємо: доки картинка їде (або
// якщо не приїде зовсім), у рядку має лишитися щось осмислене.
std::string extract_emotes(const std::string& content, std::vector<EmoteRef>* found) {
    if (content.find("[emote:") == std::string::npos) return content;

    std::string out;
    out.reserve(content.size());
    size_t i = 0;
    while (i < content.size()) {
        const size_t start = content.find("[emote:", i);
        if (start == std::string::npos) { out.append(content, i, std::string::npos); break; }
        out.append(content, i, start - i);

        const size_t id_beg = start + 7;
        const size_t colon = content.find(':', id_beg);
        const size_t close = content.find(']', id_beg);
        if (colon == std::string::npos || close == std::string::npos || colon > close) {
            out.append(content, start, 7);
            i = start + 7;
            continue;
        }
        const std::string id = content.substr(id_beg, colon - id_beg);
        std::string name = content.substr(colon + 1, close - colon - 1);
        if (name.empty()) name = id;
        const std::string code = ":" + name + ":";
        out += code;
        found->push_back({code, "https://files.kick.com/emotes/" + id + "/fullsize"});
        i = close + 1;
    }
    return out;
}

// Значки Kick приїжджають списком у identity.badges. Зводимо їх до спільних
// імен — тих самих, що й у Twitch, — щоб та сама людина в обох чатах виглядала
// однаково.
std::vector<std::string> badges_of(const json& identity) {
    static const std::map<std::string, std::string> kKnown = {
        {"broadcaster", "broadcaster"}, {"moderator", "mod"}, {"vip", "vip"},
        {"subscriber", "sub"}, {"founder", "sub"}, {"verified", "verified"},
        {"staff", "staff"}, {"og", "og"},
    };
    std::vector<std::string> out;
    auto b = identity.find("badges");
    if (b == identity.end() || !b->is_array()) return out;
    for (const auto& one : *b) {
        if (!one.is_object()) continue;
        auto it = kKnown.find(str_of(one, "type"));
        if (it == kKnown.end()) continue;
        bool have = false;
        for (const auto& n : out) if (n == it->second) { have = true; break; }
        if (!have) out.push_back(it->second);
    }
    return out;
}

// Ключ автора. Рахується в одному місці, бо той самий ключ приходить і в
// заборонi: інакше «прибрати все від X» не знайшло б жодного рядка.
std::string nick_of(const json& sender) {
    std::string nick = str_of(sender, "username");
    size_t a = 0, b = nick.size();
    while (a < b && (unsigned char)nick[a] <= ' ') ++a;
    while (b > a && (unsigned char)nick[b - 1] <= ' ') --b;
    nick = nick.substr(a, b - a);
    for (char& c : nick) {
        if (c == ' ') c = '_';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    if (!nick.empty()) return nick;
    std::string slug = str_of(sender, "slug");
    for (char& c : slug) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return slug;
}

// Прибрати пробіли з обох боків.
std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

}  // namespace

KickSource::KickSource() = default;

KickSource::~KickSource() { stop(); }

bool KickSource::start(const std::string& channel, ChatSink sink) {
    channel_ = channel;
    sink_ = std::move(sink);
    {
        std::lock_guard<std::mutex> lock(ping_mx_);
        stopping_ = false;
    }
    if (channel_.empty()) { error_ = "не задано канал"; return false; }

    ix::initNetSystem();

    // Крок 1: у самого Kick питаємо номер кімнати. Без нього підписуватися нема
    // на що. Тут же дізнаємося id каналу — його чекає 7TV.
    const HttpResult r = http_get("https://kick.com/api/v2/channels/" + channel_, 20);
    if (!r.ok()) {
        error_ = r.status ? ("Kick відповів " + std::to_string(r.status))
                          : ("Kick недоступний: " + r.error);
        return false;
    }
    try {
        const json d = json::parse(r.body);
        channel_id_ = str_of(d, "id");
        auto room = d.find("chatroom");
        if (room != d.end() && room->is_object()) chatroom_id_ = str_of(*room, "id");
    } catch (const std::exception& e) {
        error_ = std::string("відповідь Kick не розібралася: ") + e.what();
        return false;
    }
    if (chatroom_id_.empty()) { error_ = "у каналу немає чату"; return false; }

    // Набори сторонніх емоутів тягнемо наперед і збоку: перше повідомлення
    // має прийти вже з картинками, але чекати на це під'єднання не мусить.
    {
        const std::string cid = channel_id_;
        std::thread([cid] { emotes().warm("kick", cid); }).detach();
    }

    // Крок 2: Pusher.
    ws_.reset(new ix::WebSocket());
    ws_->setUrl(kPusherUrl);
    // Pusher рве тихі з'єднання. Стукаємо в нього рівно так, як стукає його
    // власний клієнт у сторінці Kick, — своїм «pusher:ping», а не лише
    // службовим ping самого WebSocket.
    ws_->setPingInterval(100);
    ix::SocketTLSOptions tls;
    tls.caFile = ca_bundle_path();
    ws_->setTLSOptions(tls);

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            json sub;
            sub["event"] = "pusher:subscribe";
            sub["data"] = {{"auth", ""}, {"channel", "chatrooms." + chatroom_id_ + ".v2"}};
            ws_->send(sub.dump());
            connected_ = true;
            return;
        }
        if (msg->type == ix::WebSocketMessageType::Close) { connected_ = false; return; }
        if (msg->type == ix::WebSocketMessageType::Error) {
            error_ = msg->errorInfo.reason;
            connected_ = false;
            return;
        }
        if (msg->type == ix::WebSocketMessageType::Message) on_frame(msg->str);
    });
    ws_->start();
    ping_ = std::thread([this] { ping_loop(); });
    return true;
}

// Pusher рве з'єднання, у якому довго тихо. Його власний клієнт у сторінці
// Kick шле «pusher:ping» кожні сто секунд — робимо так само.
void KickSource::ping_loop() {
    for (;;) {
        std::unique_lock<std::mutex> lock(ping_mx_);
        ping_cv_.wait_for(lock, std::chrono::seconds(100), [this] { return stopping_; });
        if (stopping_) return;
        lock.unlock();
        if (ws_ && connected_) {
            json ping;
            ping["event"] = "pusher:ping";
            ping["data"] = json::object();
            ws_->send(ping.dump());
        }
    }
}

void KickSource::on_frame(const std::string& raw) {
    json frame;
    try {
        frame = json::parse(raw);
    } catch (const std::exception&) {
        return;
    }
    const std::string name = str_of(frame, "event");

    if (name == "pusher:ping") {
        json pong;
        pong["event"] = "pusher:pong";
        pong["data"] = json::object();
        ws_->send(pong.dump());
        return;
    }

    // Ось та сама пастка: «data» — це РЯДОК із JSON усередині.
    json data = json::object();
    auto it = frame.find("data");
    if (it != frame.end() && it->is_string()) {
        try {
            data = json::parse(it->get<std::string>());
        } catch (const std::exception&) {
            return;
        }
    } else if (it != frame.end() && it->is_object()) {
        data = *it;
    }
    if (!sink_) return;

    if (name == kEvtMessage) {
        auto sender = data.find("sender");
        if (sender == data.end() || !sender->is_object()) return;
        const std::string user = str_of(*sender, "username");
        if (user.empty()) return;

        std::vector<EmoteRef> found;
        const std::string text = trimmed(extract_emotes(str_of(data, "content"), &found));
        if (text.empty()) return;

        // Сторонні емоути: у Kick це лише 7TV, і саме за id КАНАЛУ.
        found = emotes().append(found, "kick", channel_id_, text);

        ChatEvent ev;
        ev.msg.platform = "kick";
        ev.msg.id = str_of(data, "id");
        ev.msg.nick = nick_of(*sender);
        ev.msg.name = user;
        ev.msg.text = text;
        auto identity = sender->find("identity");
        if (identity != sender->end() && identity->is_object()) {
            ev.msg.color = str_of(*identity, "color");
            ev.msg.badges = badges_of(*identity);
            ev.msg.badge_icons = badges().kick(ev.msg.badges);
        }
        auto meta = data.find("metadata");
        if (meta != data.end() && meta->is_object()) {
            auto orig = meta->find("original_sender");
            if (orig != meta->end() && orig->is_object())
                ev.msg.reply = str_of(*orig, "username");
        }
        for (const auto& e : found) ev.msg.emotes.push_back({e.code, e.url});
        sink_(ev);
        return;
    }

    if (name == kEvtDeleted) {
        auto m = data.find("message");
        if (m == data.end() || !m->is_object()) return;
        ChatEvent ev;
        ev.type = ChatEvent::Type::Delete;
        ev.id = str_of(*m, "id");
        if (!ev.id.empty()) sink_(ev);
        return;
    }
    if (name == kEvtBanned) {
        auto u = data.find("user");
        if (u == data.end() || !u->is_object()) return;
        ChatEvent ev;
        ev.type = ChatEvent::Type::Purge;
        ev.nick = nick_of(*u);
        if (!ev.nick.empty()) sink_(ev);
        return;
    }

    // Решта — системні рядки. Текст складаємо тут, бо мова інтерфейсу наша.
    ChatEvent ev;
    ev.msg.platform = "kick";
    ev.msg.kind = "system";
    if (name == kEvtSub) {
        const std::string user = str_of(data, "username");
        const std::string months = str_of(data, "months");
        ev.msg.event = "sub";
        ev.msg.text = (user.empty() ? "Хтось" : user) + " підписався" +
                      (months.empty() || months == "0" ? "" : " (" + months + " міс.)");
    } else if (name == kEvtGift) {
        const std::string gifter = str_of(data, "gifter_username");
        size_t n = 1;
        auto lst = data.find("gifted_usernames");
        if (lst != data.end() && lst->is_array() && !lst->empty()) n = lst->size();
        ev.msg.event = "gift";
        ev.msg.text = (gifter.empty() ? "Хтось" : gifter) + " подарував " +
                      std::to_string(n) + " підписк(и)";
    } else if (name == kEvtPinned) {
        auto m = data.find("message");
        if (m == data.end() || !m->is_object()) return;
        std::vector<EmoteRef> ignore;
        const std::string text = trimmed(extract_emotes(str_of(*m, "content"), &ignore));
        if (text.empty()) return;
        std::string who;
        auto sender = m->find("sender");
        if (sender != m->end() && sender->is_object()) who = str_of(*sender, "username");
        ev.msg.event = "pin";
        ev.msg.text = "Закріплено (" + who + "): " + text;
    } else if (name == kEvtReward) {
        const std::string user = str_of(data, "username");
        const std::string title = str_of(data, "reward_title");
        if (user.empty() || title.empty()) return;
        ev.msg.event = "points";
        ev.msg.text = user + " витратив бали: " + title;
    } else if (name == kEvtHost) {
        const std::string host = str_of(data, "host_username");
        if (host.empty()) return;
        ev.msg.event = "raid";
        ev.msg.text = host + " привів рейд: " + str_of(data, "number_viewers") + " глядачів";
    } else {
        return;
    }
    sink_(ev);
}

void KickSource::stop() {
    {
        std::lock_guard<std::mutex> lock(ping_mx_);
        stopping_ = true;
    }
    ping_cv_.notify_all();
    if (ping_.joinable()) ping_.join();
    if (ws_) {
        ws_->stop();
        ws_.reset();
    }
    connected_ = false;
}

}  // namespace hominka
