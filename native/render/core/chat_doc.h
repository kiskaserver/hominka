// Складання розмітки одного повідомлення — порт hominka/feed/page.py.
//
// Чому ОДНЕ повідомлення, а не вся стрічка одним документом: рядок, який уже
// намальовано, більше не змінюється. Склавши кожен окремо, ми розкладаємо й
// растеризуємо лише новий, а решту показуємо готовими картинками. У браузері
// так було не можна — там документ один; тут можна, і саме звідси береться
// «чат більше не молотить у холосту».
//
// Другий наслідок того ж рішення: поява рядка малюється нативно (стрічка сама
// зсуває й проявляє готовий кадр), тож @keyframes, якого litehtml не знає,
// нам і не потрібен.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace hominka {

// «Чи можна вже намалювати картинку за цією адресою».
//
// Потрібне, бо картинки приходять асинхронно: значок або емоут може ще не
// докачатися, а деякі не малюються зовсім (SVG із <text>). Без цього на їх
// місці лишалася б дірка. З ним плашка автора падає назад на текстову
// («MOD», «OG»), а емоут — на свій код, тобто на те, як чат виглядав до появи
// справжніх іконок. Порожньо не буває ніколи.
//
// Порожня функція означає «усе доступне» — так зручно в перевірках.
using ImageReady = std::function<bool(const std::string& url)>;

// Емоут у тексті: код («Kappa») і адреса картинки.
struct Emote {
    std::string code;
    std::string url;
};

// Значок автора зі справжньою іконкою (Twitch/Kick/YouTube). Якщо іконки для
// значка немає — малюється текстова плашка з BADGE_LABELS.
struct BadgeIcon {
    std::string id;
    std::string url;
};

// Повідомлення — те саме, що будує hominka/chatsources.py:message().
struct ChatMessage {
    std::string id;
    std::string platform;          // twitch | kick | youtube | site
    std::string kind;              // "" (звичайне) | "system"
    std::string event;             // raid | sub | gift | announce | pin | ...
    std::string nick;
    std::string name;
    std::string color;
    std::string text;
    std::string reply;             // нік, кому відповідають
    std::string amount;            // «200 UAH», «1000 bits» — лише в платних
    std::vector<std::string> badges;
    std::vector<BadgeIcon> badge_icons;
    std::vector<Emote> emotes;
};

// Порядок частин рядка. Приходить від програми (chatLayout); вимкнена частина
// позначена мінусом («-reply»). Невідомі імена мовчки пропускаємо: чужий чи
// застарілий config.json не має ламати чат.
std::vector<std::string> clean_layout(const std::vector<std::string>& layout);

// Розмітка одного повідомлення: <div class="m" data-…>…</div>.
// Повідомлення з того, що прислав Python. Це рівно той dict, який будує
// hominka/chatsources.py:message(), і розбирати його треба однаково на всіх
// системах — інакше чат на Linux показував би не те саме, що на Windows.
ChatMessage message_from_json(const nlohmann::json& j);

std::string message_html(const ChatMessage& m, const std::vector<std::string>& layout,
                         const ImageReady& ready = ImageReady());

// Повний документ під одне повідомлення: базові стилі + свій CSS користувача +
// сам рядок. Саме його віддаємо litehtml.
std::string message_document(const ChatMessage& m, const std::vector<std::string>& layout,
                             const std::string& user_css,
                             const ImageReady& ready = ImageReady());

// Екранування тексту, що йде в розмітку. Те саме, що esc() на сторінці.
std::string html_escape(const std::string& s);

}  // namespace hominka
