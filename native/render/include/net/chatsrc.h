// Спільна мова для всіх джерел чату.
//
// Площадок три, і кожна говорить по-своєму: Twitch — IRC, Kick — Pusher,
// YouTube — свій внутрішній JSON. Але стрічці байдуже, звідки рядок: їй
// потрібне або нове повідомлення, або вказівка прибрати вже показане. Саме це
// й описано тут, а весь протокольний бруд лишається всередині джерела.
#pragma once

#include <functional>
#include <string>

#include "core/chat_doc.h"

namespace hominka {

// Що прийшло з площадки. Повідомлення — окремо, бо їх найбільше; решта —
// вказівки прибрати вже показане.
struct ChatEvent {
    enum class Type { Message, Delete, Purge, Clear };
    Type type = Type::Message;
    ChatMessage msg;             // для Message
    std::string id;              // для Delete
    std::string nick;            // для Purge
    // Clear — модератор почистив увесь чат: стрічка спорожняється.
};

// Джерело кличе це на кожну подію, з ЧУЖОГО потоку. Той, хто передає sink,
// відповідає за те, щоб усередині було безпечно.
using ChatSink = std::function<void(const ChatEvent&)>;

}  // namespace hominka
