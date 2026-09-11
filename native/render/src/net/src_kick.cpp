#include "net/src_kick.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "net/badges.h"
#include "net/emotes.h"
#include "net/net_http.h"

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
const char* kEvtUnbanned = "App\\Events\\UserUnbannedEvent";
const char* kEvtClear    = "App\\Events\\ChatroomClearEvent";
const char* kEvtUnpinned = "App\\Events\\PinnedMessageDeletedEvent";
const char* kEvtLucky    = "App\\Events\\LuckyUsersWhoGotGiftSubscriptionsEvent";
const char* kEvtMove     = "App\\Events\\ChatMoveToSupportedChannelEvent";
const char* kEvtRoom     = "App\\Events\\ChatroomUpdatedEvent";
const char* kEvtPoll     = "App\\Events\\PollUpdateEvent";
const char* kEvtPollEnd  = "App\\Events\\PollDeleteEvent";
const char* kEvtLive     = "App\\Events\\StreamerIsLive";
const char* kEvtOffline  = "App\\Events\\StopStreamBroadcast";
const char* kEvtKicks    = "App\\Events\\KicksGifted";

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

// Прапорець, який Kick іноді шле числом, а іноді булевим.
bool bool_of(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number_integer()) return it->get<long long>() != 0;
    return false;
}

// Перелічити кількох людей одним рядком: «A, B і ще 3». Повний список у чаті
// нечитабельний, а перші імена — саме те, що ведучий назве вголос.
std::string few(const json& list, size_t show = 3) {
    if (!list.is_array() || list.empty()) return "";
    std::string out;
    size_t shown = 0;
    for (const auto& one : list) {
        std::string name;
        if (one.is_string()) name = one.get<std::string>();
        else if (one.is_object()) name = str_of(one, "username");
        if (name.empty()) continue;
        if (shown) out += ", ";
        out += name;
        if (++shown == show) break;
    }
    if (list.size() > shown)
        out += " і ще " + std::to_string(list.size() - shown);
    return out;
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
            // Три підписки, бо Kick розкладає події по різних каналах:
            // повідомлення й модерація — у кімнаті «.v2», режими чату й
            // закріплення — у кімнаті без суфікса, а «стрім почався» й переїзд
            // чату — у каналі. Підписані лише на перший, ми половини не бачили.
            const char* kRooms[] = {"chatrooms.%s.v2", "chatrooms.%s", nullptr};
            for (int i = 0; kRooms[i]; ++i) {
                char room[96];
                snprintf(room, sizeof room, kRooms[i], chatroom_id_.c_str());
                json sub;
                sub["event"] = "pusher:subscribe";
                sub["data"] = {{"auth", ""}, {"channel", room}};
                ws_->send(sub.dump());
            }
            if (!channel_id_.empty()) {
                json sub;
                sub["event"] = "pusher:subscribe";
                sub["data"] = {{"auth", ""}, {"channel", "channel." + channel_id_}};
                ws_->send(sub.dump());
            }
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
        // Кімната підписана двічі (v1 і v2), тож той самий рядок може прийти
        // двома кадрами. Пам'ятаємо останні id — дешевше, ніж показати двійника.
        const std::string mid = str_of(data, "id");
        if (!mid.empty()) {
            for (const std::string& old : recent_)
                if (old == mid) return;
            recent_.push_back(mid);
            if (recent_.size() > 64) recent_.pop_front();
        }
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
        if (ev.nick.empty()) return;
        sink_(ev);

        // Після чистки — рядок про саму дію: інакше повідомлення просто
        // зникають, і ведучий не бачить, що модератор когось спинив.
        const std::string who = str_of(*u, "username");
        // Тайм-аут відрізняється від бана лише тим, що в нього є кінець.
        const bool forever = str_of(data, "expires_at").empty();
        ChatEvent note;
        note.msg.platform = "kick";
        note.msg.kind = "system";
        note.msg.event = "ban";
        note.msg.text = (who.empty() ? ev.nick : who) +
                        (forever ? " — бан" : " — тайм-аут");
        sink_(note);
        return;
    }
    if (name == kEvtClear) {
        // Модератор почистив увесь чат.
        ChatEvent ev;
        ev.type = ChatEvent::Type::Clear;
        sink_(ev);
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
    } else if (name == kEvtLucky) {
        // Кому саме дісталися подарункові підписки з купи.
        auto lst = data.find("usernames");
        if (lst == data.end()) lst = data.find("gifted_usernames");
        const std::string who = lst == data.end() ? "" : few(*lst);
        if (who.empty()) return;
        ev.msg.event = "gift";
        ev.msg.text = "Подарункові підписки дісталися: " + who;
    } else if (name == kEvtUnbanned) {
        auto u = data.find("user");
        const std::string who = u != data.end() && u->is_object() ? str_of(*u, "username") : "";
        if (who.empty()) return;
        ev.msg.event = "unban";
        ev.msg.text = who + " — бан знято";
    } else if (name == kEvtUnpinned) {
        ev.msg.event = "unpin";
        ev.msg.text = "Закріплене повідомлення знято";
    } else if (name == kEvtMove) {
        const std::string slug = str_of(data, "channel");
        ev.msg.event = "redirect";
        ev.msg.text = slug.empty() ? "Чат переїхав на інший канал"
                                   : "Чат переїхав на канал " + slug;
    } else if (name == kEvtRoom) {
        // Режими чату приходять усі разом, станом «як зараз». Показуємо лише
        // ввімкнені: рядок «повільний вимкнено, лише підписники вимкнено…»
        // нікому не потрібен.
        struct Mode { const char* key; const char* what; const char* unit; };
        static const Mode kModes[] = {
            {"emotes_mode", "лише емоути", ""},
            {"subscribers_mode", "лише для підписників", ""},
            {"followers_mode", "лише для тих, хто стежить", " хв"},
            {"slow_mode", "повільний режим", " с"},
            {"advanced_bot_protection", "захист від ботів", ""},
            {"account_age", "обмеження за віком акаунта", " хв"},
        };
        std::string on;
        for (const Mode& m : kModes) {
            auto it = data.find(m.key);
            if (it == data.end() || !it->is_object()) continue;
            if (!bool_of(*it, "enabled")) continue;
            if (!on.empty()) on += ", ";
            on += m.what;
            const std::string n = str_of(*it, m.unit[0] ? "min_duration" : "value");
            if (*m.unit && !n.empty() && n != "0") on += " (" + n + m.unit + ")";
        }
        ev.msg.event = "mode";
        ev.msg.text = on.empty() ? "Режими чату вимкнено" : "Режими чату: " + on;
    } else if (name == kEvtPoll) {
        auto poll = data.find("poll");
        if (poll == data.end() || !poll->is_object()) return;
        const std::string title = trimmed(str_of(*poll, "title"));
        if (title.empty()) return;
        ev.msg.event = "poll";
        ev.msg.text = "Опитування: " + title;
    } else if (name == kEvtPollEnd) {
        ev.msg.event = "poll";
        ev.msg.text = "Опитування завершено";
    } else if (name == kEvtKicks) {
        // Kicks — власна валюта Kick, місцевий відповідник бітів. Поля шукаємо
        // обережно й у кількох місцях: подія молода, і Kick уже міняв її вигляд.
        auto sender = data.find("sender");
        std::string who = sender != data.end() && sender->is_object()
                              ? str_of(*sender, "username")
                              : str_of(data, "username");
        std::string n = str_of(data, "amount");
        auto gift = data.find("gift");
        if (gift != data.end() && gift->is_object()) {
            if (n.empty()) n = str_of(*gift, "amount");
            if (n.empty()) n = str_of(*gift, "quantity");
        }
        if (n.empty()) n = str_of(data, "kicks");
        if (n.empty() || n == "0") {
            if (getenv("HOMINKA_KICK_DEBUG"))
                fprintf(stderr, "[kick] KicksGifted без суми: %s\n", data.dump().c_str());
            return;
        }
        ev.msg.event = "bits";
        ev.msg.amount = n + " kicks";
        ev.msg.text = (who.empty() ? "Хтось" : who) + " надіслав " + n + " kicks";
        const std::string note = trimmed(str_of(data, "message"));
        if (!note.empty()) ev.msg.text += ": " + note;
    } else if (name == kEvtLive || name == kEvtOffline) {
        ev.msg.event = "live";
        ev.msg.text = name == kEvtLive ? "Трансляція почалася" : "Трансляція завершилася";
    } else {
        // Kick додає події мовчки й без оголошень. Щоб наступна прогалина
        // знайшлася за хвилину, а не за реліз, незнайоме ім'я видно одразу.
        // Службові кадри самого Pusher подіями чату, звісно, не є.
        if (getenv("HOMINKA_KICK_DEBUG") && name.compare(0, 6, "pusher") != 0)
            fprintf(stderr, "[kick] незнайома подія: %s\n", name.c_str());
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
