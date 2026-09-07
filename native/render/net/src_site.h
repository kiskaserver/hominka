// Чат свого сайту — тим самим websocket, що й сама сторінка чату.
//
// Довго здавалося, що це та єдина річ, заради якої доведеться тримати
// браузер: у налаштуваннях вписують ПОСИЛАННЯ НА СТОРІНКУ, а сторінку малює
// браузер. Насправді ж сторінка сама читає звичайний websocket поруч, за /ws, —
// і нам потрібен саме він, а не верстка. Тож чат сайту тут таке саме джерело,
// як Twitch чи Kick, і працює всюди: і у вікні, і в грі.
//
// Домовленість про повідомлення майже збігається з нашою — нік, колір, значки,
// іконки, емоути, кому відповідають. Це не випадковість: модель повідомлення в
// програмі з самого початку робилася дзеркалом серверної.
//
// Ключів не треба: /ws — публічний бік для глядачів, той самий, яким
// користується сторінка чату. Приватний бік із донатами й віджетами ми не
// чіпаємо.
#pragma once

#include <atomic>
#include <memory>
#include <set>
#include <string>

#include "net/chatsrc.h"

namespace ix { class WebSocket; }

namespace hominka {

// З адреси сторінки чату — адреса його websocket. Порожньо, якщо адреса ні на
// що не схожа.
std::string site_ws_url(const std::string& page_url);

class SiteSource {
public:
    SiteSource();
    ~SiteSource();

    // skip — площадки, які ми читаємо САМІ. Сервер уміє мостити Twitch і
    // YouTube у свій чат; якби ми брали і звідти, і звідси, кожне таке
    // повідомлення з'являлося б двічі.
    bool start(const std::string& page_url, const std::set<std::string>& skip,
               ChatSink sink);
    void stop();

    bool connected() const { return connected_; }
    const std::string& error() const { return error_; }

private:
    void on_text(const std::string& raw);

    std::unique_ptr<ix::WebSocket> ws_;
    ChatSink sink_;
    std::set<std::string> skip_;
    std::string error_;
    std::atomic<bool> connected_{false};
};

}  // namespace hominka
