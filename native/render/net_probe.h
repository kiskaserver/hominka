// Перша перевірка мережі в C++: під'єднатися до чату Twitch і прочитати його.
//
// Навіщо окремо й наперед. Перенесення мережі з Python — це не «переписати
// класи», а поява ЗДАТНОСТІ, якої в нативній частині не було зовсім: TLS,
// WebSocket, розбір IRC. Якщо вона не працює, решта переносу безглузда, тож
// перевіряємо її першою й проти справжнього сервера, а не проти макета.
//
// Читаємо анонімно: Twitch дозволяє під'єднатися ніком «justinfan<число>» без
// жодного токена, і для чату, який ми лише показуємо, цього досить. Токен
// знадобиться пізніше — щоб бачити підписки й бали каналу.
#pragma once

#include <functional>
#include <string>

namespace hominka {

// Одне повідомлення чату, як його віддав IRC.
struct ProbeMessage {
    std::string nick;
    std::string name;      // display-name, якщо є
    std::string color;     // «#RRGGBB», якщо є
    std::string text;
};

// Під'єднується до каналу й кличе on_msg на кожне повідомлення, доки не мине
// seconds. Повертає, скільки повідомлень прочитано, або -1, якщо з'єднатися не
// вдалося взагалі.
int net_probe_twitch(const std::string& channel, int seconds,
                     const std::function<void(const ProbeMessage&)>& on_msg);

}  // namespace hominka
