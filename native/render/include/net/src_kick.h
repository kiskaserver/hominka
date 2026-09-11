// Чат Kick.
//
// Kick не має власного протоколу чату — він користується Pusher, готовим
// сервісом розсилки подій. Тому шлях такий: спитати в самого Kick номер
// кімнати каналу (звичайний HTTP), під'єднатися до Pusher і підписатися на
// «chatrooms.<номер>.v2». Далі події приходять кадрами JSON.
//
// Ключ Pusher зашитий у сторінці самого Kick і не є секретом — це публічний
// ідентифікатор застосунку, той самий для всіх глядачів.
//
// Одна пастка, через яку тут легко помилитися: поле «data» у кадрі Pusher —
// це JSON-РЯДОК усередині JSON, а не вкладений об'єкт. Його треба розібрати
// вдруге, і забути про це означає отримати порожні повідомлення без жодної
// помилки.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "net/chatsrc.h"

namespace ix { class WebSocket; }

namespace hominka {

class KickSource {
public:
    // Обидва оголошено, а не за замовчуванням: ix::WebSocket тут лише названий,
    // і unique_ptr на неповний тип вимагає, щоб і створення, і знищення були в
    // .cpp, де тип уже відомий.
    KickSource();
    ~KickSource();

    // Під'єднується й починає слати події в sink. Виклик не блокує: пошук
    // кімнати й саме з'єднання йдуть у власному потоці IXWebSocket.
    bool start(const std::string& channel, ChatSink sink);
    void stop();

    // Номер каналу (не кімнати) — саме його чекає 7TV для набору Kick.
    const std::string& channel_id() const { return channel_id_; }
    bool connected() const { return connected_; }
    const std::string& error() const { return error_; }

private:
    void on_frame(const std::string& raw);
    void ping_loop();

    std::unique_ptr<ix::WebSocket> ws_;
    ChatSink sink_;
    std::string channel_;
    std::string channel_id_;
    std::string chatroom_id_;
    std::string error_;
    std::atomic<bool> connected_{false};

    // Останні побачені id повідомлень — захист від двійників. Живуть лише в
    // потоці сокета, тому без замка.
    std::deque<std::string> recent_;

    // Власний стукіт у Pusher. Чекаємо на змінній, а не спимо шматками: тоді
    // stop() завершується миттєво, а не за хвилину з гаком.
    std::thread ping_;
    std::mutex ping_mx_;
    std::condition_variable ping_cv_;
    bool stopping_ = false;
};

}  // namespace hominka
