#include "ui/samples.h"

#include "net/badges.h"

namespace hominka {

namespace {

// Ті самі три емоути, що в Qt-довіднику (hominka/cssui/catalog.py): усміхнене
// коло, серце й вогник. Прості SVG, бо вони мають бути видні одразу, без
// жодного запиту в мережу.
const char* kSmile =
    "data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='28' height='28'>"
    "<circle cx='14' cy='14' r='13' fill='%23fbbf24'/><circle cx='9' cy='11' r='2' fill='%23111'/>"
    "<circle cx='19' cy='11' r='2' fill='%23111'/><path d='M8 18q6 5 12 0' stroke='%23111' "
    "stroke-width='2' fill='none' stroke-linecap='round'/></svg>";
const char* kHeart =
    "data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='28' height='28'>"
    "<path d='M14 24C4 17 2 11 6 7q4-3 8 2 4-5 8-2c4 4 2 10-8 17z' fill='%23f472b6'/></svg>";
const char* kFire =
    "data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='28' height='28'>"
    "<path d='M14 2c1 5-4 6-4 11a4 4 0 008 0c0-2-1-3-1-3 3 1 4 4 4 7a7 7 0 11-14 0c0-6 5-8 7-15z' "
    "fill='%23fb923c'/></svg>";

ChatMessage msg(const char* platform, const char* nick, const char* name,
                const char* color, const char* text) {
    ChatMessage m;
    m.platform = platform;
    m.nick = nick;
    m.name = name;
    m.color = color;
    m.text = text;
    return m;
}

}  // namespace

std::vector<ChatMessage> demo_messages() {
    std::vector<ChatMessage> out;

    {
        ChatMessage m = msg("twitch", "goodtheme", "GoodTheme", "#ff7f50",
                            "о, привіт! Kappa як воно?");
        m.badges = {"mod", "sub"};
        m.emotes = {{"Kappa", kSmile}};
        out.push_back(m);
    }
    {
        // Звертання в тексті навмисно немає: його малює позначка «↳ нік», і
        // дублювати його в рядку — той самий баг, який ми прибрали в самому
        // чаті. Зразок мусить показувати правильне.
        ChatMessage m = msg("kick", "lazar1n", "Lazar1n", "#53fc18", "та нормально, дивимось");
        m.reply = "GoodTheme";
        m.badges = {"broadcaster"};
        m.badge_icons = badges().kick(m.badges);
        out.push_back(m);
    }
    {
        ChatMessage m = msg("youtube", "yt:UC_demo", "Мандрівниця", "#38bdf8",
                            "дякую за стрім! <3 було дуже цікаво");
        m.badges = {"member"};
        m.emotes = {{"<3", kHeart}};
        out.push_back(m);
    }
    {
        ChatMessage m = msg("twitch", "bigfan", "BigFan", "#a855f7", "тримай на каву!");
        m.kind = "";
        m.event = "bits";
        m.amount = "500 bits";
        m.badges = {"vip"};
        m.emotes = {{"PogFire", kFire}};
        out.push_back(m);
    }
    {
        ChatMessage m = msg("kick", "raider", "RaidLeader", "", "");
        m.kind = "system";
        m.event = "raid";
        m.text = "RaidLeader привів рейд: 128 глядачів";
        out.push_back(m);
    }
    {
        ChatMessage m = msg("twitch", "newsub", "NewSub", "", "");
        m.kind = "system";
        m.event = "sub";
        m.text = "NewSub підписався на 3 місяці поспіль";
        out.push_back(m);
    }
    {
        ChatMessage m = msg("twitch", "longpost", "ДовгийРядок", "#22c55e",
                            "перевіряю, як тема поводиться з довгим текстом, який точно "
                            "не влізе в один рядок і має перенестися сам, разом із "
                            "посиланням https://example.com/дуже/довгий/шлях/сторінки");
        out.push_back(m);
    }
    return out;
}

}  // namespace hominka
