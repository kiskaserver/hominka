// Скільки зараз дивиться.
//
// Число не приходить разом із чатом: у чаті його немає в жодної з площадок.
// Тому це окремий, рідкісний опит — раз на хвилину, у власному потоці. Частіше
// не треба: лічильник глядачів і сам оновлюється приблизно так, а стукати в
// три сервіси щосекунди заради цифри, яка змінюється на одиниці, — грубо.
//
// Кожна площадка каже це по-своєму:
//   Twitch  — GraphQL, той самий публічний ключ клієнта, що й для значків;
//   Kick    — та сама довідка про канал, з якої ми беремо номер кімнати;
//   YouTube — сторінка трансляції, бо окремого способу спитати немає:
//             «originalViewCount» поруч із «isLive».
//
// Незнання й нуль — різні речі. Канал може бути не в ефірі, сервіс може не
// відповісти; у таких випадках ми НЕ показуємо нуль, а не показуємо нічого.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace hominka {

class Viewers {
public:
    ~Viewers();

    struct Count {
        bool known = false;
        int n = 0;
    };

    // Канали в тому ж вигляді, що й у ChatNet: уже без «twitch.tv/».
    void configure(const std::string& twitch, const std::string& kick,
                   const std::string& youtube);
    void stop();

    Count twitch() const;
    Count kick() const;
    Count youtube() const;
    // Сума відомих. known=false, якщо не знаємо жодного.
    Count total() const;

private:
    void run();
    bool wait(int seconds);

    mutable std::mutex mx_;
    std::string twitch_ch_, kick_ch_, youtube_ch_;
    Count twitch_, kick_, youtube_;

    std::thread worker_;
    std::condition_variable cv_;
    std::mutex wake_mx_;
    bool stopping_ = false;
    bool poke_ = false;              // канали змінилися — не чекати хвилину
};

// «60866» → «60 866». Пробіли між тисячами: чотиризначне число без них
// читається як рік, а шестизначне — не читається зовсім.
std::string group_digits(int n);

}  // namespace hominka
