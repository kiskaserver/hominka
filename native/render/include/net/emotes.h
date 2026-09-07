// Сторонні емоути: 7TV, BetterTTV, FrankerFaceZ.
//
// Що це таке. Площадки знають лише свої емоути; усе, чим глядачі насправді
// пишуть, живе в цих трьох сервісах. Кожен віддає два набори: ЗАГАЛЬНИЙ (той
// самий для всіх) і КАНАЛЬНИЙ (те, що додав стример). Ми беремо обидва й
// складаємо в одну таблицю «слово → картинка».
//
// Підміна робиться по СЛОВАХ, а не пошуком підрядка. Емоут «D:» усередині
// «MyD:name» — не емоут, і замінити його там означало б порізати чуже слово
// картинкою. Саме тому таблиця, а не регулярний вираз.
//
// Мережа тут синхронна й викликається з окремого потоку: набір тягнеться раз
// на під'єднання до каналу, тримати заради цього асинхронність ні до чого.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hominka {

// Скільки тримаємо набір, перш ніж питати сервіс знову. Стример додає емоути
// не щохвилини, а от зайвий запит на кожне повідомлення був би відчутний.
static const int64_t EMOTES_TTL_GLOBAL_MS = 6 * 60 * 60 * 1000;   // 6 годин
static const int64_t EMOTES_TTL_CHANNEL_MS = 30 * 60 * 1000;      // 30 хвилин

struct EmoteRef {
    std::string code;
    std::string url;
};

class Emotes {
public:
    // Додає до вже знайдених (площадкових) емоутів ті, які трапилися в тексті.
    // platform — «twitch» | «kick» | «youtube», channel_id — числовий id каналу
    // тієї площадки (для Twitch це тег room-id, для Kick — id каналу).
    //
    // Уже наявні коди не чіпаємо: емоут самої площадки має перевагу над
    // стороннім із такою ж назвою.
    std::vector<EmoteRef> append(const std::vector<EmoteRef>& have,
                                 const std::string& platform,
                                 const std::string& channel_id,
                                 const std::string& text);

    // Наперед підтягнути набори, щоб перше повідомлення вже було з картинками.
    void warm(const std::string& platform, const std::string& channel_id);

private:
    struct Set {
        std::map<std::string, std::string> by_code;   // код → URL
        int64_t fetched_ms = 0;
        bool loading = false;
    };

    const std::map<std::string, std::string>& global();
    const std::map<std::string, std::string>& channel(const std::string& platform,
                                                      const std::string& cid);

    static void fetch_global(std::map<std::string, std::string>* out);
    static void fetch_channel(const std::string& platform, const std::string& cid,
                              std::map<std::string, std::string>* out);

    std::mutex mx_;
    Set global_;
    std::map<std::string, Set> channels_;   // «platform:id» → набір
};

// Один на програму: набори однакові для всіх, а тягнути їх двічі — марно.
Emotes& emotes();

}  // namespace hominka
