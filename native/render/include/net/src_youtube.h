// Чат YouTube — через InnerTube, той самий внутрішній API, яким користується
// сама сторінка YouTube. Ключів не треба, добової квоти в нього немає (на
// відміну від YouTube Data API).
//
// Тут немає жодного сокета: чат опитується звичайними запитами, а YouTube сам
// каже, коли прийти наступного разу (timeoutMs). Тому джерело крутить власний
// потік, а не живе всередині зворотного виклику, як Twitch і Kick.
//
// Три речі, на яких уже спіткалися — і на сервері, і в Python-версії:
//   * беремо режим «Live chat», а не «Top chat»: у другому YouTube ховає
//     частину повідомлень, і глядач просто не з'являється в чаті;
//   * токен цього режиму треба брати зі сторінки поп-ауту чату — на сторінці
//     watch він обрізаний, і get_live_chat відповідає на нього 400;
//   * суперчат — це донат: сума важливіша за текст, а стикер узагалі буває без
//     тексту, і викидати «порожні» повідомлення означає губити гроші.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "net/chatsrc.h"

namespace hominka {

class YouTubeSource {
public:
    ~YouTubeSource();

    // channel — «@нік», «UC…» або посилання на трансляцію. Ефіру може не бути
    // зовсім: це не помилка, джерело просто чекає й перевіряє знову.
    bool start(const std::string& channel, ChatSink sink);
    void stop();

    bool connected() const { return connected_; }
    const std::string& error() const { return error_; }

private:
    void run();
    void read(const std::string& video);
    // Чекає до seconds або до stop(). true — час вийшов, false — час іти.
    bool wait(double seconds);

    ChatSink sink_;
    std::string channel_;
    std::string error_;
    std::atomic<bool> connected_{false};

    std::thread thread_;
    std::mutex mx_;
    std::condition_variable cv_;
    bool stopping_ = false;
};

}  // namespace hominka
