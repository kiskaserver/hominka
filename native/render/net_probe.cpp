#include "net_probe.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXWebSocket.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <unistd.h>
#endif

namespace hominka {

namespace {

// Розбір тегів IRCv3: «@key=value;key2=value2 :nick!... PRIVMSG #chan :текст».
//
// Значення в тегах екрановані власним способом (\s — пробіл, \: — крапка з
// комою), і не розкодувати їх означало б показувати «Nice\sname» замість
// «Nice name».
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

std::string tag_value(const std::string& tags, const std::string& key) {
    size_t pos = 0;
    while (pos < tags.size()) {
        const size_t eq = tags.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = tags.find(';', eq);
        if (end == std::string::npos) end = tags.size();
        if (tags.compare(pos, eq - pos, key) == 0)
            return unescape_tag(tags.substr(eq + 1, end - eq - 1));
        pos = end + 1;
    }
    return "";
}

// Один рядок IRC у повідомлення. false — це не PRIVMSG (пінг, службове тощо).
bool parse_privmsg(const std::string& line, ProbeMessage* out) {
    size_t i = 0;
    std::string tags;
    if (!line.empty() && line[0] == '@') {
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) return false;
        tags = line.substr(1, sp - 1);
        i = sp + 1;
    }
    if (i >= line.size() || line[i] != ':') return false;

    const size_t sp2 = line.find(' ', i);
    if (sp2 == std::string::npos) return false;
    const std::string prefix = line.substr(i + 1, sp2 - i - 1);
    const size_t bang = prefix.find('!');
    const std::string nick = bang == std::string::npos ? prefix : prefix.substr(0, bang);

    const size_t cmd_end = line.find(' ', sp2 + 1);
    if (cmd_end == std::string::npos) return false;
    if (line.compare(sp2 + 1, cmd_end - sp2 - 1, "PRIVMSG") != 0) return false;

    const size_t colon = line.find(" :", cmd_end);
    if (colon == std::string::npos) return false;

    out->nick = nick;
    out->text = line.substr(colon + 2);
    out->name = tag_value(tags, "display-name");
    out->color = tag_value(tags, "color");
    if (out->name.empty()) out->name = nick;
    // Хвостовий \r лишається від роздільника рядків IRC.
    while (!out->text.empty() && (out->text.back() == '\r' || out->text.back() == '\n'))
        out->text.pop_back();
    return true;
}

// Кому довіряти. Повертає або ШЛЯХ до файлу, або самі сертифікати текстом —
// IXWebSocket приймає обидва (рядок, що починається з «-----BEGIN», він
// вважає вмістом, а не шляхом).
//
// mbedTLS, на відміну від системних бібліотек, не знає жодного сховища: йому
// треба дати корені явно. Без них рукостискання не відбувається взагалі, а
// помилка виглядає як «не вдалося під'єднатися» — тобто вказує кудись не туди.
// На це вже пішла година.
//
// Перевірку НЕ вимикаємо. Спокуса є (чат публічний, що там красти), але через
// це з'єднання чужий у мережі зможе підмінити вміст чату, а показуємо ми його
// на весь екран.
std::string ca_bundle() {
#ifdef _WIN32
    // Беремо сховище самої Windows, а не власний cacert.pem поруч із програмою.
    // Так ми, по-перше, не тягнемо файл, який через рік протухне й почне тихо
    // ламати з'єднання, а по-друге — поважаємо корені, які додав користувач
    // або його організація.
    static std::string pem;
    if (!pem.empty()) return pem;

    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) return "SYSTEM";
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = CertEnumCertificatesInStore(store, ctx)) != nullptr) {
        DWORD chars = 0;
        if (!CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded,
                                  CRYPT_STRING_BASE64HEADER, nullptr, &chars))
            continue;
        std::string one(chars, '\0');
        if (CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded,
                                 CRYPT_STRING_BASE64HEADER, &one[0], &chars)) {
            one.resize(chars);
            pem += one;
        }
    }
    CertCloseStore(store, 0);
    // Порожньо — краще чесно віддати «SYSTEM» і дати бібліотеці сказати своє,
    // ніж мовчки лишитися без перевірки.
    return pem.empty() ? std::string("SYSTEM") : pem;
#else
    static const char* kCandidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",   // Debian, Ubuntu, Alpine
        "/etc/pki/tls/certs/ca-bundle.crt",     // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",               // openSUSE
        "/etc/ssl/cert.pem",                    // Alpine, macOS
    };
    for (const char* p : kCandidates) {
        FILE* f = fopen(p, "rb");
        if (f) { fclose(f); return p; }
    }
    return "SYSTEM";
#endif
}

}  // namespace

int net_probe_twitch(const std::string& channel, int seconds,
                     const std::function<void(const ProbeMessage&)>& on_msg) {
    // Під Windows сокети треба підняти явно (WSAStartup); під Linux це нічого
    // не робить, але кличеться однаково — щоб не було двох шляхів.
    ix::initNetSystem();

    ix::WebSocket ws;
    ws.setUrl("wss://irc-ws.chat.twitch.tv:443");
    // Пінг раз на 20 секунд: Twitch мовчки рве з'єднання, яке нічого не шле, а
    // чат буває тихим годинами.
    ws.setPingInterval(20);

    ix::SocketTLSOptions tls;
    tls.caFile = ca_bundle();
    ws.setTLSOptions(tls);

    std::atomic<int> count{0};
    std::atomic<bool> joined{false};
    std::atomic<bool> failed{false};
    std::string tail;                    // недочитаний хвіст останнього кадру
    std::mutex tail_mx;

    ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            // Анонімний вхід: пароль будь-який, нік «justinfan<число>».
            ws.send("CAP REQ :twitch.tv/tags twitch.tv/commands");
            ws.send("PASS SCHMOOPIIE");
            ws.send("NICK justinfan64537");
            ws.send("JOIN #" + channel);
            joined = true;
            return;
        }
        if (msg->type == ix::WebSocketMessageType::Error) {
            fprintf(stderr, "мережа: %s\n", msg->errorInfo.reason.c_str());
            failed = true;
            return;
        }
        if (msg->type != ix::WebSocketMessageType::Message) return;

        // Кадр WebSocket не дорівнює рядку IRC: в одному кадрі їх буває кілька,
        // а останній може прийти розрізаним. Тому склеюємо через хвіст.
        std::string data;
        {
            std::lock_guard<std::mutex> lock(tail_mx);
            tail += msg->str;
            const size_t last = tail.rfind('\n');
            if (last == std::string::npos) return;
            data = tail.substr(0, last + 1);
            tail.erase(0, last + 1);
        }

        size_t pos = 0;
        while (pos < data.size()) {
            size_t nl = data.find('\n', pos);
            if (nl == std::string::npos) nl = data.size();
            const std::string line = data.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.empty()) continue;

            // На PING треба відповісти тим самим PONG, інакше нас відключать.
            if (line.compare(0, 4, "PING") == 0) {
                ws.send("PONG :tmi.twitch.tv");
                continue;
            }
            ProbeMessage m;
            if (parse_privmsg(line, &m)) {
                ++count;
                if (on_msg) on_msg(m);
            }
        }
    });

    ws.start();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until && !failed)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ws.stop();
    ix::uninitNetSystem();

    if (failed && count == 0) return -1;
    if (!joined) return -1;
    return count.load();
}

}  // namespace hominka
