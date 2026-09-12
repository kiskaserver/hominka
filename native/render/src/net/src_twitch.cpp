#include "net/src_twitch.h"

#include <cstdlib>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include "net/badges.h"
#include "net/emotes.h"
#include "net/net_http.h"

namespace hominka {

namespace {

const char* kIrcUrl = "wss://irc-ws.chat.twitch.tv:443";
const char* kEmoteCdn = "https://static-cdn.jtvnw.net/emoticons/v2/";
const char* kCheerCdn = "https://d3aqoihi2n8ty8.cloudfront.net/actions/";

// Значення тегів екрановані власним способом (\s — пробіл, \: — крапка з
// комою). Не розкодувати їх означало б показувати «Nice\sname».
std::string unescape_tag(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\' || i + 1 >= v.size()) { out += v[i]; continue; }
        switch (v[++i]) {
        case 's': out += ' '; break;
        case ':': out += ';'; break;
        case 'r': out += '\r'; break;
        case 'n': out += '\n'; break;
        case '\\': out += '\\'; break;
        default: out += v[i]; break;
        }
    }
    return out;
}

using Tags = std::map<std::string, std::string>;

Tags parse_tags(const std::string& raw) {
    Tags out;
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t end = raw.find(';', pos);
        const bool last = end == std::string::npos;
        if (last) end = raw.size();
        const std::string part = raw.substr(pos, end - pos);
        pos = end + 1;
        if (!part.empty()) {
            const size_t eq = part.find('=');
            if (eq == std::string::npos) out[part] = "";
            else out[part.substr(0, eq)] = unescape_tag(part.substr(eq + 1));
        }
        if (last) break;
    }
    return out;
}

std::string tag(const Tags& t, const char* key) {
    auto it = t.find(key);
    return it == t.end() ? std::string() : it->second;
}

// Рядок IRC → теги, префікс, команда, параметри.
void parse_line(const std::string& line, Tags* tags, std::string* prefix,
                std::string* cmd, std::vector<std::string>* params) {
    std::string s = line;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();

    if (!s.empty() && s[0] == '@') {
        const size_t sp = s.find(' ');
        *tags = parse_tags(sp == std::string::npos ? s.substr(1) : s.substr(1, sp - 1));
        s = sp == std::string::npos ? std::string() : s.substr(sp + 1);
    }
    if (!s.empty() && s[0] == ':') {
        const size_t sp = s.find(' ');
        *prefix = sp == std::string::npos ? s.substr(1) : s.substr(1, sp - 1);
        s = sp == std::string::npos ? std::string() : s.substr(sp + 1);
    }
    const size_t sp = s.find(' ');
    *cmd = s.substr(0, sp);
    for (char& c : *cmd) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    std::string rest = sp == std::string::npos ? std::string() : s.substr(sp + 1);

    while (!rest.empty()) {
        if (rest[0] == ':') { params->push_back(rest.substr(1)); break; }
        const size_t next = rest.find(' ');
        params->push_back(rest.substr(0, next));
        if (next == std::string::npos) break;
        rest = rest.substr(next + 1);
    }
}

// Регалії Twitch → спільний набір імен (той самий, що й у Kick).
std::vector<std::string> map_badges(const std::string& raw) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t end = raw.find(',', pos);
        const bool last = end == std::string::npos;
        if (last) end = raw.size();
        const std::string part = raw.substr(pos, end - pos);
        pos = end + 1;
        if (!part.empty()) {
            const size_t slash = part.find('/');
            const std::string name =
                twitch_badge_name(slash == std::string::npos ? part : part.substr(0, slash));
            // Ім'я набору порожнім більше не буває, та перевірку лишаємо:
            // порожній «badges=» у тезі трапляється.
            if (!name.empty()) {
                bool have = false;
                for (const auto& n : out) if (n == name) { have = true; break; }
                if (!have) out.push_back(name);
            }
        }
        if (last) break;
    }
    return out;
}

// Початки кодових позицій UTF-8. Саме в них Twitch рахує діапазони емоутів, і
// саме через це наївне s.substr(a, b - a) ріже кирилицю навпіл.
std::vector<size_t> cp_offsets(const std::string& s) {
    std::vector<size_t> out;
    for (size_t i = 0; i < s.size(); ++i)
        if ((s[i] & 0xC0) != 0x80) out.push_back(i);
    out.push_back(s.size());
    return out;
}

// Тег emotes: «25:0-4,12-16/1902:6-10» → картинки.
std::vector<Emote> parse_emotes(const std::string& raw, const std::string& text) {
    std::vector<Emote> out;
    if (raw.empty() || text.empty()) return out;
    const std::vector<size_t> cp = cp_offsets(text);
    const size_t len = cp.size() - 1;

    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t end = raw.find('/', pos);
        const bool last = end == std::string::npos;
        if (last) end = raw.size();
        const std::string item = raw.substr(pos, end - pos);
        pos = end + 1;

        const size_t colon = item.find(':');
        if (colon != std::string::npos && colon > 0) {
            const std::string id = item.substr(0, colon);
            // Беремо лише перший діапазон: код емоута в решті той самий.
            std::string first = item.substr(colon + 1);
            const size_t comma = first.find(',');
            if (comma != std::string::npos) first = first.substr(0, comma);
            const size_t dash = first.find('-');
            if (dash != std::string::npos) {
                const long a = strtol(first.substr(0, dash).c_str(), nullptr, 10);
                const long b = strtol(first.substr(dash + 1).c_str(), nullptr, 10);
                if (a >= 0 && b >= a && (size_t)b < len)
                    out.push_back({text.substr(cp[a], cp[b + 1] - cp[a]),
                                   std::string(kEmoteCdn) + id + "/default/dark/2.0"});
            }
        }
        if (last) break;
    }
    return out;
}

bool digits(const std::string& s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

// Біти або платне закріплене повідомлення. Гроші окремим полем: показати їх
// звичайним рядком означало б загубити донат серед іншого чату.
std::string money(const Tags& t) {
    const std::string bits = tag(t, "bits");
    if (digits(bits) && strtol(bits.c_str(), nullptr, 10) > 0) return bits + " bits";

    const std::string raw = tag(t, "pinned-chat-paid-amount");
    const std::string cur = tag(t, "pinned-chat-paid-currency");
    if (!digits(raw) || cur.empty()) return "";
    const std::string exp = tag(t, "pinned-chat-paid-exponent");
    const long e = exp.empty() ? 0 : strtol(exp.c_str(), nullptr, 10);
    if (e < 0 || e > 9) return "";
    double value = (double)strtol(raw.c_str(), nullptr, 10);
    for (long i = 0; i < e; ++i) value /= 10.0;

    char buf[64];
    snprintf(buf, sizeof buf, "%g %s", value, cur.c_str());
    return buf;
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool blank(const std::string& s) {
    for (char c : s) if ((unsigned char)c > ' ') return false;
    return true;
}

// Twitch щоразу вигадує нові приводи: ювілей модератора, віха глядача, біти
// за значок. Ловити кожен окремо — гнатися за рухомою ціллю, тож незнайому
// подію ми все одно показуємо (текстом самого Twitch), а оформлення підбираємо
// за назвою. Гірше, ніж влучне ім'я, але незрівнянно краще за мовчання.
std::string event_of(const std::string& kind) {
    const auto has = [&kind](const char* s) { return kind.find(s) != std::string::npos; };
    if (has("raid")) return "raid";
    if (has("bits")) return "bits";
    if (has("milestone") || has("versary")) return "milestone";
    if (has("gift")) return "gift";
    if (has("sub") || has("pay") || has("member")) return "sub";
    if (has("announce")) return "announce";
    return "";
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

// Черимоути: «Cheer100», «Party1000» — це не слова, а картинки, і без них
// повідомлення з бітами читається як набір літер із числом. У тегу emotes їх
// немає: Twitch віддає набір черимоутів окремо, за ключем застосунку, якого в
// нас немає й не буде — ми читаємо чат анонімно. Зате самі картинки лежать
// відкрито, а ім'я файлу складається за простим правилом «префікс + рівень»,
// тож адресу збираємо самі.
//
// Рівнів рівно п'ять, і сума округлюється вниз до найближчого: за «/3/» CDN
// віддає 403. Якщо префікс усе-таки виявиться чужим (канали заводять власні),
// картинка не завантажиться — і в рядку лишиться сам код, як було до того.
std::vector<EmoteRef> cheermotes(const std::string& text) {
    std::vector<EmoteRef> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (unsigned char)text[i] <= ' ') ++i;
        const size_t start = i;
        while (i < text.size() && (unsigned char)text[i] > ' ') ++i;
        const std::string word = text.substr(start, i - start);

        // Слово має бути рівно «літери + число».
        size_t d = 0;
        while (d < word.size() && !(word[d] >= '0' && word[d] <= '9')) ++d;
        if (d == 0 || d == word.size() || !digits(word.substr(d))) continue;
        bool letters = true;
        for (size_t k = 0; k < d; ++k)
            if (!((word[k] >= 'a' && word[k] <= 'z') || (word[k] >= 'A' && word[k] <= 'Z')))
                letters = false;
        if (!letters) continue;
        const long n = strtol(word.c_str() + d, nullptr, 10);
        if (n <= 0) continue;

        bool have = false;
        for (const EmoteRef& r : out) if (r.code == word) have = true;
        if (have) continue;

        static const long kTiers[] = {10000, 5000, 1000, 100, 1};
        long tier = 1;
        for (long t : kTiers) if (n >= t) { tier = t; break; }
        out.push_back({word, std::string(kCheerCdn) + lower(word.substr(0, d)) +
                                 "/dark/static/" + std::to_string(tier) + "/2.png"});
    }
    return out;
}

}  // namespace

TwitchSource::TwitchSource() = default;
TwitchSource::~TwitchSource() { stop(); }

std::string TwitchSource::room_id() {
    std::lock_guard<std::mutex> lock(mx_);
    return room_id_;
}

bool TwitchSource::start(const std::string& channel, ChatSink sink) {
    channel_ = lower(channel);
    while (!channel_.empty() && channel_[0] == '#') channel_.erase(0, 1);
    sink_ = std::move(sink);
    if (channel_.empty()) { error_ = "не задано канал"; return false; }

    ix::initNetSystem();
    ws_.reset(new ix::WebSocket());
    ws_->setUrl(kIrcUrl);
    // Пінг раз на 20 секунд: Twitch мовчки рве з'єднання, яке нічого не шле, а
    // чат буває тихим годинами.
    ws_->setPingInterval(20);
    ix::SocketTLSOptions tls;
    tls.caFile = ca_bundle_path();
    ws_->setTLSOptions(tls);

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            // Анонімний вхід: пароль будь-який, нік «justinfan<число>». Писати
            // ми не можемо й не хочемо — нам треба лише читати.
            ws_->send("CAP REQ :twitch.tv/tags twitch.tv/commands");
            ws_->send("PASS SCHMOOPIIE");
            ws_->send("NICK justinfan64537");
            ws_->send("JOIN #" + channel_);
            connected_ = true;
            return;
        }
        if (msg->type == ix::WebSocketMessageType::Close) { connected_ = false; return; }
        if (msg->type == ix::WebSocketMessageType::Error) {
            error_ = msg->errorInfo.reason;
            connected_ = false;
            return;
        }
        if (msg->type == ix::WebSocketMessageType::Message) {
            // Сирі кадри під рукою: коли чат «мовчить», з першого погляду не
            // видно, чи ми не під'єдналися, чи не зайшли в канал, чи не
            // розібрали рядок. HOMINKA_IRC_DEBUG=1 показує це за секунду.
            if (getenv("HOMINKA_IRC_DEBUG")) fprintf(stderr, "[irc] %s", msg->str.c_str());
            on_text(msg->str);
        }
    });
    ws_->start();
    return true;
}

void TwitchSource::on_text(const std::string& frame) {
    // Кадр WebSocket не дорівнює рядку IRC — склеюємо через хвіст.
    std::string data;
    {
        std::lock_guard<std::mutex> lock(mx_);
        tail_ += frame;
        const size_t last = tail_.rfind('\n');
        if (last == std::string::npos) return;
        data = tail_.substr(0, last + 1);
        tail_.erase(0, last + 1);
    }
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) nl = data.size();
        const std::string line = data.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty()) handle_line(line);
    }
}

void TwitchSource::handle_line(const std::string& line) {
    // На PING треба відповісти тим самим PONG, інакше нас відключать.
    if (line.compare(0, 4, "PING") == 0) {
        ws_->send("PONG :tmi.twitch.tv");
        return;
    }
    Tags tags;
    std::string prefix, cmd;
    std::vector<std::string> params;
    parse_line(line, &tags, &prefix, &cmd, &params);
    if (!sink_) return;

    if (cmd == "RECONNECT") {
        // Twitch попереджає, що зараз відключить. Рвемо самі — під'єднається
        // IXWebSocket, він це вміє й сам.
        ws_->close();
        return;
    }
    if (cmd == "CLEARMSG") {
        ChatEvent ev;
        ev.type = ChatEvent::Type::Delete;
        ev.id = tag(tags, "target-msg-id");
        if (!ev.id.empty()) sink_(ev);
        return;
    }
    if (cmd == "CLEARCHAT") {
        // З іменем — бан або тайм-аут одного глядача; без імені — модератор
        // почистив увесь чат. Друге ми раніше мовчки пропускали, і стрічка
        // лишалася з рядками, яких у чаті вже немає.
        const std::string who = params.size() > 1 ? lower(trimmed(params[1])) : "";
        ChatEvent ev;
        if (who.empty()) {
            ev.type = ChatEvent::Type::Clear;
        } else {
            ev.type = ChatEvent::Type::Purge;
            ev.nick = who;
        }
        sink_(ev);
        return;
    }
    if (cmd == "ROOMSTATE") {
        room_state(tags);
        return;
    }

    if (cmd == "PRIVMSG") {
        if (params.size() < 2) return;
        std::string text = params[1];
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
        // «/me» приходить обгорнутим у службові байти CTCP.
        const char kAction[] = "\x01" "ACTION ";
        if (text.size() > 8 && text.compare(0, 8, kAction) == 0 && text.back() == '\x01')
            text = "/me " + text.substr(8, text.size() - 9);

        const std::string amount = money(tags);
        if (blank(text) && amount.empty()) return;

        const size_t bang = prefix.find('!');
        const std::string login = bang == std::string::npos ? prefix : prefix.substr(0, bang);
        const std::string room = tag(tags, "room-id");

        // Канал узнаємо лише звідси, з першого ж повідомлення: набори емоутів і
        // значків тягнемо один раз і збоку, щоб не тримати чат.
        {
            std::lock_guard<std::mutex> lock(mx_);
            if (!room.empty() && !warmed_) {
                room_id_ = room;
                warmed_ = true;
                std::thread([room] {
                    emotes().warm("twitch", room);
                    badges().warm(room);
                }).detach();
            }
        }

        ChatEvent ev;
        ev.msg.platform = "twitch";
        ev.msg.id = tag(tags, "id");
        ev.msg.nick = login;
        ev.msg.name = tag(tags, "display-name");
        if (ev.msg.name.empty()) ev.msg.name = login;
        ev.msg.color = tag(tags, "color");
        ev.msg.text = text;
        ev.msg.reply = tag(tags, "reply-parent-display-name");
        ev.msg.amount = amount;
        // Порядок навмисний: у платного рядка гроші переважають усе інше, а
        // «перше повідомлення» цікаве лише тоді, коли іншого приводу немає.
        if (!amount.empty()) ev.msg.event = "bits";
        else if (tag(tags, "msg-id") == "highlighted-message") ev.msg.event = "highlight";
        else if (!tag(tags, "custom-reward-id").empty()) ev.msg.event = "points";
        else if (tag(tags, "first-msg") == "1") ev.msg.event = "first";
        else if (tag(tags, "returning-chatter") == "1") ev.msg.event = "returning";
        ev.msg.badges = map_badges(tag(tags, "badges"));
        ev.msg.badge_icons = badges().twitch(room, tag(tags, "badges"));

        // Рідні емоути Twitch (тег emotes) плюс сторонні, знайдені в тексті.
        std::vector<EmoteRef> refs;
        for (const auto& e : parse_emotes(tag(tags, "emotes"), text))
            refs.push_back({e.code, e.url});
        // Черимоути шукаємо лише там, де біти справді є: інакше будь-яке
        // «Kappa100» у звичайному рядку перетворилося б на картинку.
        if (!tag(tags, "bits").empty())
            for (const EmoteRef& c : cheermotes(text)) refs.push_back(c);
        refs = emotes().append(refs, "twitch", room, text);
        for (const auto& r : refs) ev.msg.emotes.push_back({r.code, r.url});

        sink_(ev);
        return;
    }

    if (cmd == "USERNOTICE") {
        // Підписки, рейди, оголошення — усе, що Twitch шле системним.
        std::string user = tag(tags, "display-name");
        if (user.empty()) user = tag(tags, "login");
        if (user.empty()) user = "Anonymous";
        const std::string kind = tag(tags, "msg-id");
        const std::string body = trimmed(params.size() > 1 ? params[1] : "");

        ChatEvent ev;
        ev.msg.platform = "twitch";
        ev.msg.kind = "system";
        // Свій текст події Twitch майже завжди складає сам (system-msg), і він
        // уже перекладений мовою каналу. Складаємо самі лише там, де його немає.
        const std::string msg = trimmed(tag(tags, "system-msg"));
        if (kind == "raid") {
            const std::string n = tag(tags, "msg-param-viewerCount");
            ev.msg.event = "raid";
            ev.msg.text = msg.empty()
                              ? user + " привів рейд: " + (n.empty() ? "?" : n) + " глядачів"
                              : msg;
        } else if (kind == "unraid") {
            ev.msg.event = "raid";
            ev.msg.text = msg.empty() ? user + " скасував рейд" : msg;
        } else if (kind == "announcement") {
            if (body.empty()) return;
            ev.msg.event = "announce";
            ev.msg.text = user + ": " + body;
        } else if (kind == "viewermilestone" || kind == "modiversary") {
            // Віха глядача (тижні поспіль) і річниця модератора. Текст у них
            // свій, а привід спільний — обидва варто помітити, не переплутавши
            // з підпискою.
            ev.msg.event = "milestone";
            ev.msg.text = msg.empty() ? user + ": віха" : msg;
            if (!body.empty()) ev.msg.text += ": " + body;
        } else if (kind == "bitsbadgetier") {
            ev.msg.event = "bits";
            ev.msg.text = msg.empty() ? user + ": новий значок за біти" : msg;
        } else if (kind == "rewardgift") {
            ev.msg.event = "gift";
            ev.msg.text = msg.empty() ? user + ": подарунок глядачам" : msg;
        } else if (kind == "sub" || kind == "resub" || kind == "subgift" ||
                   kind == "anonsubgift" || kind == "submysterygift" ||
                   kind == "anonsubmysterygift" || kind == "primepaidupgrade" ||
                   kind == "giftpaidupgrade" || kind == "anongiftpaidupgrade" ||
                   kind == "standardpayforward" || kind == "communitypayforward") {
            ev.msg.text = msg.empty() ? user + ": підписка" : msg;
            // Підписник часто пише кілька слів «від себе» — вони приходять
            // тілом рядка, і саме їх читає ведучий уголос.
            if (!blank(body)) ev.msg.text += ": " + trimmed(body);
            // Подарунок і власна підписка — різні приводи, і оформлюють їх
            // по-різному: одне вітають, друге дякують.
            ev.msg.event = kind.find("gift") != std::string::npos ||
                                   kind.find("forward") != std::string::npos
                               ? "gift"
                               : "sub";
        } else {
            ev.msg.event = event_of(kind);
            ev.msg.text = msg;
            if (msg.empty() && !blank(body)) ev.msg.text = user + ": " + trimmed(body);
            if (getenv("HOMINKA_IRC_DEBUG"))
                fprintf(stderr, "[irc] незнайома подія USERNOTICE: %s\n", kind.c_str());
        }
        ev.msg.text = trimmed(ev.msg.text);
        if (!ev.msg.text.empty()) sink_(ev);
        return;
    }
}

// Режими чату (лише емоути, лише підписники, повільний, без повторів, лише
// для тих, хто стежить). Перший ROOMSTATE приходить одразу після входу й
// описує весь стан — його ми лише запам'ятовуємо: розповідати про те, що й так
// було, нема сенсу. Далі Twitch шле тільки те, що змінилося, і ось про це вже
// варто сказати. Викликається з потоку сокета, як і решта розбору.
void TwitchSource::room_state(const Tags& tags) {
    struct Mode { const char* tag; const char* what; const char* unit; };
    static const Mode kModes[] = {
        {"emote-only", "лише емоути", ""},
        {"subs-only", "лише для підписників", ""},
        {"r9k", "без повторів", ""},
        {"followers-only", "лише для тих, хто стежить", " хв"},
        {"slow", "повільний режим", " с"},
    };
    for (const Mode& m : kModes) {
        const std::string now = tag(tags, m.tag);
        if (now.empty()) continue;  // цього разу не змінювалося
        auto it = modes_.find(m.tag);
        const bool known = it != modes_.end();
        if (known && it->second == now) continue;
        modes_[m.tag] = now;
        if (!known) continue;  // перший ROOMSTATE — просто запам'ятали

        const long n = strtol(now.c_str(), nullptr, 10);
        // Нуль і -1 однаково означають «вимкнено»; додатне число в режимах з
        // мірою — саму міру (хвилини стеження, секунди затримки).
        std::string text = n <= 0 ? std::string("Вимкнено: ") + m.what
                                  : std::string("Увімкнено: ") + m.what;
        if (n > 0 && *m.unit) text += " (" + now + m.unit + ")";
        ChatEvent ev;
        ev.msg.platform = "twitch";
        ev.msg.kind = "system";
        ev.msg.event = "mode";
        ev.msg.text = text;
        if (sink_) sink_(ev);
    }
}

void TwitchSource::stop() {
    if (ws_) {
        ws_->stop();
        ws_.reset();
    }
    connected_ = false;
}

}  // namespace hominka
