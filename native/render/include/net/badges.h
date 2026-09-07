// Справжні іконки значків автора.
//
// Текстова плашка («MOD», «VIP») у стрічці була завжди; тут — картинки, які
// площадки віддають самі. Де картинки немає, плашка лишається, тож порожнього
// місця не буває ніколи.
//
// Twitch віддає два набори: глобальний і канальний. Канальний перебиває
// глобальний — саме там живуть значки підписки з номером місяця, які стример
// намалював сам. Питаємо їх у GraphQL: колишній відкритий badges.twitch.tv
// більше не існує (див. badges.cpp).
//
// У Kick іконок у чаті немає зовсім, тому їх ми несемо з собою: ті самі
// вбудовані SVG, що й на сервері. Усі вони — самі лише <path>, і це не
// випадковість: нативний рендер растеризує їх через nanosvg, у якого немає
// шрифтового рушія, тож значок із <text> вийшов би порожньою плиткою.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/chat_doc.h"

namespace hominka {

class Badges {
public:
    // Сирий тег Twitch («subscriber/12,moderator/1») + room-id каналу.
    // По одній іконці на нормалізоване ім'я: два значки з однією плашкою — це
    // одна плашка, а не дві.
    std::vector<BadgeIcon> twitch(const std::string& room_id, const std::string& tag);

    // Уже нормалізовані значки Kick → вбудовані картинки. Мережі не треба.
    std::vector<BadgeIcon> kick(const std::vector<std::string>& norm);

    // Підтягнути набори наперед, щоб перше повідомлення вже було зі значками.
    void warm(const std::string& room_id);

private:
    struct Set {
        std::map<std::string, std::string> by_key;   // «набір/версія» → URL
        int64_t fetched_ms = 0;
    };

    const std::map<std::string, std::string>& global();
    const std::map<std::string, std::string>& channel(const std::string& room_id);

    std::mutex mx_;
    Set global_;
    std::map<std::string, Set> channels_;
};

// Нормалізоване ім'я значка Twitch («moderator» → «mod») або порожньо, якщо
// значок нам невідомий. Той самий перелік, що й у текстових плашках.
std::string twitch_badge_name(const std::string& set_id);

Badges& badges();

}  // namespace hominka
