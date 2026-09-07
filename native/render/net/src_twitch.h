// Чат Twitch — анонімно, без ключів і без входу.
//
// Twitch віддає IRC поверх WebSocket, і читати чат може будь-хто: досить
// представитися ніком виду justinfan<число>. Теги IRCv3 дають усе інше —
// значки, колір ніка, емоути, біти, id повідомлення.
//
// Дві речі, на яких тут легко спіткнутися (і на яких уже спіткалися):
//   * діапазони емоутів рахуються в СИМВОЛАХ, а не байтах — інакше кирилиця
//     зсуває вирізку, і замість емоута дістається сміття;
//   * кадр WebSocket не дорівнює рядку IRC: в одному кадрі їх буває кілька, а
//     останній приходить розрізаним, тож рядки треба склеювати через хвіст.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "net/chatsrc.h"

namespace ix { class WebSocket; }

namespace hominka {

class TwitchSource {
public:
    TwitchSource();
    ~TwitchSource();

    bool start(const std::string& channel, ChatSink sink);
    void stop();

    // Числовий id каналу (тег room-id). Приходить із першим повідомленням —
    // за ним тягнуться набори сторонніх емоутів і значків.
    std::string room_id();
    bool connected() const { return connected_; }
    const std::string& error() const { return error_; }

private:
    void on_text(const std::string& frame);
    void handle_line(const std::string& line);

    std::unique_ptr<ix::WebSocket> ws_;
    ChatSink sink_;
    std::string channel_;
    std::string error_;
    std::atomic<bool> connected_{false};

    std::mutex mx_;
    std::string tail_;        // недочитаний хвіст останнього кадру
    std::string room_id_;
    bool warmed_ = false;     // набори тягнемо один раз, коли взнали канал
};

}  // namespace hominka
