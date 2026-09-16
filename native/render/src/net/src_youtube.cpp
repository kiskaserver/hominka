#include "net/src_youtube.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include <nlohmann/json.hpp>

#include "net/emotes.h"
#include "net/net_http.h"

namespace hominka {

namespace {

using json = nlohmann::json;

const double POLL_MIN = 1.0;
const double POLL_DEFAULT = 2.0;
const double RETRY = 6.0;
const double LIVE_RECHECK = 60.0;

// Ключ і версія на випадок, якщо сторінка змінить розмітку й ми їх не знайдемо.
// Прострочені, але робочі: InnerTube приймає й старі.
const char* kFallbackKey = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
const char* kFallbackVer = "2.20240726.00.00";

// Без згоди на куки й без мови YouTube віддає іншу сторінку — з іншою
// розміткою, у якій потрібного об'єкта просто немає.
std::map<std::string, std::string> page_headers() {
    return {{"Accept-Language", "en-US,en;q=0.9"},
            {"Cookie", "CONSENT=YES+cb; PREF=hl=en"},
            {"Accept", "text/html"}};
}

std::string page(const std::string& url) {
    const HttpResult r = http_get(url, page_headers(), 20);
    // Сторінки YouTube — це мегабайт розмітки, у якій потрібне заховано. Коли
    // чат «мовчить», з першого погляду не видно, чи сторінка не приїхала, чи
    // приїхала не та. HOMINKA_YT_DEBUG=1 показує це за секунду.
    if (getenv("HOMINKA_YT_DEBUG"))
        fprintf(stderr, "[yt] %s → %d, %d байт%s\n", url.c_str(), r.status,
                (int)r.body.size(), r.error.empty() ? "" : (" (" + r.error + ")").c_str());
    return r.ok() ? r.body : std::string();
}

// «"КЛЮЧ":"значення"» у розмітці сторінки.
std::string quoted_value(const std::string& html, const std::string& key) {
    const std::string pat = "\"" + key + "\":\"";
    const size_t i = html.find(pat);
    if (i == std::string::npos) return "";
    const size_t beg = i + pat.size();
    const size_t end = html.find('"', beg);
    if (end == std::string::npos) return "";
    return html.substr(beg, end - beg);
}

// Дістає «<маркер> = {…}» зі сторінки.
//
// YouTube віддає стан сторінки не окремим API, а великим об'єктом усередині
// розмітки, і після нього в тому ж рядку йде ще купа коду. Тому рахуємо дужки,
// а не шукаємо кінець рядка.
json extract_json(const std::string& html, const std::string& marker) {
    for (const std::string& pat : {marker + " = ", marker + "\"] = "}) {
        const size_t i = html.find(pat);
        if (i == std::string::npos) continue;
        const size_t beg = i + pat.size();

        int depth = 0;
        bool in_str = false, esc = false;
        for (size_t n = beg; n < html.size(); ++n) {
            const char c = html[n];
            if (in_str) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') in_str = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                try {
                    return json::parse(html.substr(beg, n - beg + 1));
                } catch (const std::exception&) {
                    return json::object();
                }
            }
        }
        return json::object();
    }
    return json::object();
}

// Спуск по вкладених об'єктах. Порожній об'єкт, якщо шляху немає, — так
// перевірка «чи є» пишеться один раз, а не на кожному кроці.
const json& jget(const json& obj, const std::vector<const char*>& keys) {
    static const json kNull = json();
    const json* cur = &obj;
    for (const char* k : keys) {
        if (!cur->is_object()) return kNull;
        auto it = cur->find(k);
        if (it == cur->end()) return kNull;
        cur = &*it;
    }
    return *cur;
}

std::string jstr(const json& obj, const std::vector<const char*>& keys) {
    const json& v = jget(obj, keys);
    return v.is_string() ? v.get<std::string>() : std::string();
}

std::string lstrip_at(std::string s) {
    while (!s.empty() && s[0] == '@') s.erase(0, 1);
    return s;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

// Текст повідомлення + картинки власних емодзі каналу.
void runs_to_text(const json& runs, std::string* text, std::vector<Emote>* emotes) {
    if (!runs.is_array()) return;
    for (const auto& r : runs) {
        if (!r.is_object()) continue;
        auto t = r.find("text");
        if (t != r.end() && t->is_string()) { *text += t->get<std::string>(); continue; }

        const json& em = jget(r, {"emoji"});
        if (!em.is_object()) continue;
        const std::string id = jstr(em, {"emojiId"});
        auto custom = em.find("isCustomEmoji");
        const bool is_custom = custom != em.end() && custom->is_boolean() && custom->get<bool>();
        if (!id.empty() && !is_custom) {
            *text += id;                 // звичайний emoji — це сам символ
            continue;
        }
        const json& shortcuts = jget(em, {"shortcuts"});
        if (!shortcuts.is_array() || shortcuts.empty() || !shortcuts[0].is_string()) continue;
        const std::string code = shortcuts[0].get<std::string>();
        *text += code;

        // Беремо найбільшу мініатюру: у чаті вона все одно зменшиться, а от
        // збільшена дрібна виглядає мазаною.
        const json& thumbs = jget(em, {"image", "thumbnails"});
        if (!thumbs.is_array()) continue;
        std::string best;
        long best_w = -1;
        for (const auto& th : thumbs) {
            if (!th.is_object()) continue;
            const std::string url = jstr(th, {"url"});
            if (url.empty()) continue;
            auto w = th.find("width");
            const long width = (w != th.end() && w->is_number()) ? w->get<long>() : 0;
            if (width > best_w) { best_w = width; best = url; }
        }
        if (!best.empty()) emotes->push_back({code, best});
    }
}

// Значки автора → спільний набір імен.
std::vector<std::string> author_badges(const json& items) {
    std::vector<std::string> out;
    if (!items.is_array()) return out;
    for (const auto& b : items) {
        const json& r = jget(b, {"liveChatAuthorBadgeRenderer"});
        if (!r.is_object()) continue;
        const std::string icon = jstr(r, {"icon", "iconType"});
        if (icon == "OWNER") out.push_back("broadcaster");
        else if (icon == "MODERATOR") out.push_back("mod");
        else if (icon == "VERIFIED") out.push_back("verified");
        // Саме наявність ключа: в учасника каналу замість іконки картинка
        // рівня членства, і порожній об'єкт тут — теж «так, учасник».
        else if (r.find("customThumbnail") != r.end()) out.push_back("member");
    }
    return out;
}

// Справжні картинки значків. У YouTube картинка є лише в значка учасника;
// власник, модератор і підтверджений приходять типом іконки без URL — для них
// лишається текстова плашка.
std::vector<BadgeIcon> author_badge_icons(const json& items) {
    std::vector<BadgeIcon> out;
    if (!items.is_array()) return out;
    for (const auto& b : items) {
        const json& thumbs =
            jget(b, {"liveChatAuthorBadgeRenderer", "customThumbnail", "thumbnails"});
        if (!thumbs.is_array() || thumbs.empty()) continue;
        const std::string url = jstr(thumbs.back(), {"url"});
        if (!url.empty()) out.push_back({"member", url});
    }
    return out;
}

bool blank(const std::string& s) {
    for (char c : s) if ((unsigned char)c > ' ') return false;
    return true;
}

// Один елемент чату → подія. false — це щось, чого ми не показуємо.
bool parse_item(const json& item, const std::string& channel_id, ChatEvent* ev) {
    struct Kind { const char* key; bool paid; };
    static const Kind kKinds[] = {
        {"liveChatTextMessageRenderer", false},
        {"liveChatPaidMessageRenderer", true},
        {"liveChatPaidStickerRenderer", true},
    };
    for (const Kind& k : kKinds) {
        const json& r = jget(item, {k.key});
        if (!r.is_object()) continue;

        std::string text;
        std::vector<Emote> own;
        runs_to_text(jget(r, {"message", "runs"}), &text, &own);
        const std::string amount = k.paid ? jstr(r, {"purchaseAmountText", "simpleText"}) : "";
        if (blank(text) && amount.empty()) return false;

        const std::string name = lstrip_at(jstr(r, {"authorName", "simpleText"}));
        const std::string channel = jstr(r, {"authorExternalChannelId"});

        ev->msg.platform = "youtube";
        ev->msg.id = jstr(r, {"id"});
        // Ключ автора — id каналу: ніка в YouTube немає, а бан приходить саме
        // за ним, і підписати повідомлення чимось іншим означало б не знайти
        // жодного рядка при чистці.
        ev->msg.nick = channel.empty() ? name : "yt:" + channel;
        ev->msg.name = name;
        ev->msg.text = text;
        ev->msg.amount = amount;
        if (!amount.empty()) ev->msg.event = "superchat";
        ev->msg.badges = author_badges(jget(r, {"authorBadges"}));
        ev->msg.badge_icons = author_badge_icons(jget(r, {"authorBadges"}));

        // Рідні емодзі каналу (runs) + сторонні: загальні завжди, канальні —
        // коли відомий UC-id ведучого.
        std::vector<EmoteRef> refs;
        for (const auto& e : own) refs.push_back({e.code, e.url});
        refs = emotes().append(refs, "youtube", channel_id, text);
        ev->msg.emotes.clear();
        for (const auto& r2 : refs) ev->msg.emotes.push_back({r2.code, r2.url});
        return true;
    }

    ev->msg.platform = "youtube";
    ev->msg.kind = "system";

    const json& memb = jget(item, {"liveChatMembershipItemRenderer"});
    if (memb.is_object()) {
        const std::string user = lstrip_at(jstr(memb, {"authorName", "simpleText"}));
        std::string head;
        std::vector<Emote> ignore;
        runs_to_text(jget(memb, {"headerPrimaryText", "runs"}), &head, &ignore);
        const std::string sub = jstr(memb, {"headerSubtext", "simpleText"});
        // Спонсорство YouTube — та сама підписка, просто названа інакше.
        ev->msg.event = "sub";
        ev->msg.text = trimmed(user + " — " + (head.empty() ? sub : head));
        while (!ev->msg.text.empty() &&
               (ev->msg.text.back() == '-' || ev->msg.text.back() == ' '))
            ev->msg.text.pop_back();
        return !trimmed(ev->msg.text).empty();
    }

    const json& gift = jget(item, {"liveChatSponsorshipsGiftPurchaseAnnouncementRenderer",
                                   "header", "liveChatSponsorshipsHeaderRenderer"});
    if (gift.is_object()) {
        const std::string user = lstrip_at(jstr(gift, {"authorName", "simpleText"}));
        std::string text;
        std::vector<Emote> ignore;
        runs_to_text(jget(gift, {"primaryText", "runs"}), &text, &ignore);
        ev->msg.event = "gift";
        ev->msg.text = text.empty() ? user : user + " — " + text;
        return !trimmed(ev->msg.text).empty();
    }

    const json& redeem = jget(item, {"liveChatSponsorshipsGiftRedemptionAnnouncementRenderer"});
    if (redeem.is_object()) {
        const std::string user = lstrip_at(jstr(redeem, {"authorName", "simpleText"}));
        std::string text;
        std::vector<Emote> ignore;
        runs_to_text(jget(redeem, {"message", "runs"}), &text, &ignore);
        ev->msg.event = "gift";
        ev->msg.text = trimmed(user + " " + text);
        return !ev->msg.text.empty();
    }

    const json& mode = jget(item, {"liveChatModeChangeMessageRenderer"});
    if (mode.is_object()) {
        std::string text;
        std::vector<Emote> ignore;
        runs_to_text(jget(mode, {"text", "runs"}), &text, &ignore);
        ev->msg.event = "mode";
        ev->msg.text = trimmed(text);
        return !ev->msg.text.empty();
    }

    // Збір коштів: YouTube оголошує донат окремим рядком, не суперчатом.
    const json& donate = jget(item, {"liveChatDonationAnnouncementRenderer"});
    if (donate.is_object()) {
        std::string text, sub;
        std::vector<Emote> ignore;
        runs_to_text(jget(donate, {"text", "runs"}), &text, &ignore);
        runs_to_text(jget(donate, {"subtext", "runs"}), &sub, &ignore);
        ev->msg.event = "donate";
        ev->msg.text = trimmed(trimmed(text) + (sub.empty() ? "" : " — " + trimmed(sub)));
        return !ev->msg.text.empty();
    }

    // Службові підказки самого YouTube («чат у режимі повтору», «будьте
    // ввічливі»). Показуємо їх окремим приводом, щоб легко було сховати.
    const json& note = jget(item, {"liveChatViewerEngagementMessageRenderer"});
    if (note.is_object()) {
        std::string text;
        std::vector<Emote> ignore;
        runs_to_text(jget(note, {"message", "runs"}), &text, &ignore);
        ev->msg.event = "info";
        ev->msg.text = trimmed(text);
        return !ev->msg.text.empty();
    }

    // Порожнє місце під повідомлення, яке ще їде, — не подія.
    if (jget(item, {"liveChatPlaceholderItemRenderer"}).is_object()) return false;

    // YouTube міняє внутрішні назви без попередження. Замість мовчазної
    // прогалини — рядок у журналі з іменем того, чого ми не знаємо.
    if (getenv("HOMINKA_YT_DEBUG") && item.is_object() && !item.empty())
        fprintf(stderr, "[yt] незнайомий елемент: %s\n", item.begin().key().c_str());
    return false;
}

// Закріплене згори (банер): або повідомлення, або оголошення про рейд.
bool parse_banner(const json& banner, const std::string& channel_id, ChatEvent* ev) {
    const json& contents = jget(banner, {"contents"});
    ev->msg.platform = "youtube";
    ev->msg.kind = "system";

    const json& redirect = jget(contents, {"liveChatBannerRedirectRenderer"});
    if (redirect.is_object()) {
        std::string text;
        std::vector<Emote> ignore;
        runs_to_text(jget(redirect, {"bannerMessage", "runs"}), &text, &ignore);
        ev->msg.event = "raid";
        ev->msg.text = trimmed(text);
        return !ev->msg.text.empty();
    }

    // Звичайне закріплене повідомлення. Показуємо окремим системним рядком, а
    // не тим самим id: сам рядок у стрічці вже є, і підмінити його означало б
    // або втратити оригінал, або показати двійника.
    ChatEvent inner;
    if (!parse_item(contents, channel_id, &inner)) return false;
    const std::string who = inner.msg.name;
    ev->msg.event = "pin";
    ev->msg.text = trimmed("Закріплено" + (who.empty() ? "" : " (" + who + ")") + ": " +
                           inner.msg.text);
    return !ev->msg.text.empty();
}

void parse_actions(const json& actions, const std::string& channel_id,
                   std::vector<ChatEvent>* out) {
    if (!actions.is_array()) return;
    for (const auto& a : actions) {
        if (!a.is_object()) continue;

        const json& item = jget(a, {"addChatItemAction", "item"});
        if (item.is_object()) {
            ChatEvent ev;
            if (parse_item(item, channel_id, &ev)) out->push_back(ev);
            continue;
        }
        // Заміна рядка: старий прибираємо, новий показуємо як звичайний.
        const json& repl = jget(a, {"replaceChatItemAction"});
        if (repl.is_object()) {
            const std::string target = jstr(repl, {"targetItemId"});
            if (!target.empty()) {
                ChatEvent gone;
                gone.type = ChatEvent::Type::Delete;
                gone.id = target;
                out->push_back(gone);
            }
            ChatEvent ev;
            if (parse_item(jget(repl, {"replacementItem"}), channel_id, &ev))
                out->push_back(ev);
            continue;
        }

        const json& banner = jget(a, {"addBannerToLiveChatCommand", "bannerRenderer",
                                      "liveChatBannerRenderer"});
        if (banner.is_object()) {
            ChatEvent ev;
            if (parse_banner(banner, channel_id, &ev)) out->push_back(ev);
            continue;
        }
        if (jget(a, {"removeBannerForLiveChatCommand"}).is_object()) {
            ChatEvent ev;
            ev.msg.platform = "youtube";
            ev.msg.kind = "system";
            ev.msg.event = "unpin";
            ev.msg.text = "Закріплення знято";
            out->push_back(ev);
            continue;
        }

        const std::string mid = jstr(a, {"markChatItemAsDeletedAction", "targetItemId"});
        if (!mid.empty()) {
            ChatEvent ev;
            ev.type = ChatEvent::Type::Delete;
            ev.id = mid;
            out->push_back(ev);
            continue;
        }
        // Бан автора приходить з id каналу; ніка у стрічці ми не знаємо, тому
        // чистимо за тим самим ключем, яким підписуємо повідомлення.
        const std::string ch =
            jstr(a, {"markChatItemsByAuthorAsDeletedAction", "externalChannelId"});
        if (!ch.empty()) {
            ChatEvent ev;
            ev.type = ChatEvent::Type::Purge;
            ev.nick = "yt:" + ch;
            out->push_back(ev);
        }
    }
}

// Що людина вписала в поле «YouTube» → шлях на самому youtube.com.
//
// Вписати можуть що завгодно, і це нормально: «@нік», сам нік, UC-id,
// посилання на канал, посилання на трансляцію, зі «/streams» у хвості й без.
// Раніше все, що не починалося з «@» і не було рівно UC-id, приклеювалося до
// «youtube.com/@» як є — і вставлене посилання перетворювалося на
// «youtube.com/@https://www.youtube.com/@Нік/streams». Тобто поле, яке саме
// обіцяє «@нік, посилання або UC…», приймало лише перше.
std::string channel_path(const std::string& raw) {
    std::string s = trimmed(raw);
    if (s.empty()) return "";

    // Відрізаємо схему й хост, якщо вони є.
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) s.erase(0, scheme + 3);
    for (const char* host : {"www.youtube.com/", "m.youtube.com/", "youtube.com/"}) {
        const size_t n = strlen(host);
        if (s.size() > n && s.compare(0, n, host) == 0) { s.erase(0, n); break; }
    }
    while (!s.empty() && s[0] == '/') s.erase(0, 1);
    // Хвіст на кшталт «?si=…» до шляху не належить.
    const size_t q = s.find_first_of("?#");
    if (q != std::string::npos) s.erase(q);

    auto first_segment = [](const std::string& v) {
        const size_t slash = v.find('/');
        return slash == std::string::npos ? v : v.substr(0, slash);
    };

    // «channel/UC…», «c/Ім'я», «user/Ім'я» — залишаємо як є, лише без хвоста.
    for (const char* kind : {"channel/", "c/", "user/"}) {
        const size_t n = strlen(kind);
        if (s.size() > n && s.compare(0, n, kind) == 0)
            return std::string(kind) + first_segment(s.substr(n));
    }

    const std::string one = first_segment(s);
    if (one.size() == 24 && one.compare(0, 2, "UC") == 0) return "channel/" + one;
    if (one.empty()) return "";
    return "@" + lstrip_at(one);
}

// Шлях у вигляді, придатному для запиту.
//
// Ніки на YouTube бувають будь-якими літерами, і «@ДаниилКириченко-ч8к» —
// звичайний нік, а не крайній випадок. Сирі байти в адресі наш клієнт віддає
// так, що YouTube відповідає 404 (curl, до речі, з тими самими байтами
// отримує 200 — тож справа саме в нас). Кодуємо самі.
//
// Уже закодоване не чіпаємо: якщо людина скопіювала адресу з рядка браузера,
// там лежить «%D0%94…», і закодувати відсоток ще раз означало б зробити з
// нього «%25D0%2594» і той самий 404.
std::string escape_path(const std::string& path) {
    static const char* kHex = "0123456789ABCDEF";
    auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    std::string out;
    out.reserve(path.size() + 16);
    for (size_t i = 0; i < path.size(); ++i) {
        const unsigned char c = (unsigned char)path[i];
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                          c == '~' || c == '@' || c == '/';
        if (safe) { out += (char)c; continue; }
        if (c == '%' && i + 2 < path.size() && is_hex(path[i + 1]) && is_hex(path[i + 2])) {
            out.append(path, i, 3);
            i += 2;
            continue;
        }
        out += '%';
        out += kHex[c >> 4];
        out += kHex[c & 15];
    }
    return out;
}

// Канал → адреса сторінки «зараз в ефірі».
std::string live_url(const std::string& channel) {
    // Посилання на саму трансляцію: id у ньому вже є, шукати нічого.
    for (const char* mark : {"v=", "youtu.be/", "/live/"}) {
        const size_t i = channel.find(mark);
        if (i == std::string::npos) continue;
        const std::string id = channel.substr(i + strlen(mark), 11);
        if (id.size() == 11 &&
            id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "abcdefghijklmnopqrstuvwxyz0123456789_-") ==
                std::string::npos)
            return id;
    }

    const std::string path = channel_path(channel);
    if (path.empty()) return "";
    return "https://www.youtube.com/" + escape_path(path) + "/live?hl=en";
}

// Канал → id трансляції, що ЗАРАЗ в ефірі. Порожньо, якщо ефіру немає: це
// звичайний стан, а не помилка.
std::string find_live_video(const std::string& channel) {
    const std::string url = live_url(channel);
    // Посилання на саму трансляцію: id у ньому вже є, шукати нічого.
    if (url.size() == 11) return url;

    const json data = extract_json(page(url), "ytInitialData");
    if (!data.is_object()) return "";
    const std::string vid = jstr(data, {"currentVideoEndpoint", "watchEndpoint", "videoId"});
    if (vid.empty()) return "";

    // Сторінка перегляду буває і в завершеного ефіру, і в анонса — беремо лише
    // той, що ЗАРАЗ в ефірі. dump() пише без пробілів, тож шукати саме
    // «"isLive":true» тут можна (у Python на цьому вже спіткалися: там
    // json.dumps за замовчуванням ставить пробіл після двокрапки).
    return data.dump().find("\"isLive\":true") != std::string::npos ? vid : "";
}

// Ключ, версія клієнта й continuation режиму «Live chat».
bool open_session(const std::string& video, std::string* key, std::string* ver,
                  std::string* cont) {
    const std::string html =
        page("https://www.youtube.com/live_chat?v=" + video + "&is_popout=1&hl=en");
    if (html.empty()) return false;

    *key = quoted_value(html, "INNERTUBE_API_KEY");
    if (key->empty()) *key = kFallbackKey;
    *ver = quoted_value(html, "INNERTUBE_CLIENT_VERSION");
    if (ver->empty()) *ver = quoted_value(html, "clientVersion");
    if (ver->empty()) *ver = kFallbackVer;

    const json data = extract_json(html, "ytInitialData");
    const json& lcr = jget(data, {"contents", "liveChatRenderer"});
    if (!lcr.is_object()) return false;

    // Пункти йдуть [Цікавий чат, Чат наживо]; беремо останній НЕвибраний, бо
    // вибраний за замовчуванням — саме «цікавий», у якому частину повідомлень
    // сховано.
    const json& items = jget(lcr, {"header", "liveChatHeaderRenderer", "viewSelector",
                                   "sortFilterSubMenuRenderer", "subMenuItems"});
    if (items.is_array()) {
        for (size_t i = items.size(); i-- > 0;) {
            const json& it = items[i];
            if (!it.is_object()) continue;
            auto sel = it.find("selected");
            if (sel != it.end() && sel->is_boolean() && sel->get<bool>()) continue;
            const std::string c = jstr(it, {"continuation", "reloadContinuationData",
                                            "continuation"});
            if (!c.empty()) { *cont = c; break; }
        }
    }
    if (cont->empty()) {
        const json& conts = jget(lcr, {"continuations"});
        if (conts.is_array() && !conts.empty()) {
            for (const char* shape : {"invalidationContinuationData", "timedContinuationData",
                                      "reloadContinuationData"}) {
                const std::string c = jstr(conts[0], {shape, "continuation"});
                if (!c.empty()) { *cont = c; break; }
            }
        }
    }
    return !cont->empty();
}

// Один запит get_live_chat → події, наступний continuation, пауза.
bool poll_once(const std::string& key, const std::string& ver, const std::string& channel_id,
               std::string* cont, std::vector<ChatEvent>* out, double* pause) {
    json body;
    body["context"]["client"]["clientName"] = "WEB";
    body["context"]["client"]["clientVersion"] = ver;
    body["context"]["client"]["hl"] = "en";
    body["continuation"] = *cont;

    const HttpResult r = http_post(
        "https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?key=" + key +
            "&prettyPrint=false",
        body.dump(), {{"Accept-Language", "en-US,en;q=0.9"}}, 25);
    if (!r.ok()) return false;

    json data;
    try {
        data = json::parse(r.body);
    } catch (const std::exception&) {
        return false;
    }
    const json& lc = jget(data, {"continuationContents", "liveChatContinuation"});
    if (!lc.is_object()) { cont->clear(); *pause = POLL_DEFAULT; return true; }  // чат завершено

    cont->clear();
    *pause = POLL_DEFAULT;
    const json& conts = jget(lc, {"continuations"});
    if (conts.is_array() && !conts.empty()) {
        for (const char* shape : {"invalidationContinuationData", "timedContinuationData",
                                  "reloadContinuationData"}) {
            const json& node = jget(conts[0], {shape});
            if (!node.is_object()) continue;
            const std::string c = jstr(node, {"continuation"});
            if (c.empty()) continue;
            *cont = c;
            auto ms = node.find("timeoutMs");
            const double t = (ms != node.end() && ms->is_number()) ? ms->get<double>() / 1000.0 : 0;
            *pause = t > POLL_MIN ? t : POLL_MIN;
            break;
        }
    }
    parse_actions(jget(lc, {"actions"}), channel_id, out);
    return true;
}

}  // namespace

std::string youtube_live_page(const std::string& channel) {
    const std::string url = live_url(channel);
    // Коли вписали посилання на саму трансляцію, live_url повертає її id —
    // сторінку тоді складаємо самі.
    return page(url.size() == 11 ? "https://www.youtube.com/watch?v=" + url + "&hl=en"
                                 : url);
}

YouTubeSource::~YouTubeSource() { stop(); }

bool YouTubeSource::start(const std::string& channel, ChatSink sink) {
    channel_ = trimmed(channel);
    sink_ = std::move(sink);
    if (channel_.empty()) { error_ = "не задано канал"; return false; }
    {
        std::lock_guard<std::mutex> lock(mx_);
        stopping_ = false;
    }
    thread_ = std::thread([this] { run(); });
    return true;
}

bool YouTubeSource::wait(double seconds) {
    std::unique_lock<std::mutex> lock(mx_);
    cv_.wait_for(lock, std::chrono::milliseconds((long long)(seconds * 1000)),
                 [this] { return stopping_; });
    return !stopping_;
}

void YouTubeSource::run() {
    for (;;) {
        const std::string video = find_live_video(channel_);
        if (video.empty()) {
            // Ефіру немає — це нормальний стан, а не помилка.
            error_ = "";
            connected_ = false;
            if (!wait(LIVE_RECHECK)) return;
            continue;
        }
        read(video);
        if (!wait(RETRY)) return;
    }
}

void YouTubeSource::read(const std::string& video) {
    std::string key, ver, cont;
    if (!open_session(video, &key, &ver, &cont)) {
        error_ = "не вдалося відкрити чат трансляції";
        return;
    }
    error_ = "";
    connected_ = true;

    // Канальні сторонні емоути потребують UC-id ведучого. Якщо канал задано
    // саме ним — беремо як є; інакше лишаються загальні.
    const std::string uc =
        (channel_.size() == 24 && channel_.compare(0, 2, "UC") == 0) ? channel_ : std::string();
    if (!uc.empty()) emotes().warm("youtube", uc);

    bool first = true;
    while (!cont.empty()) {
        std::vector<ChatEvent> events;
        double pause = POLL_DEFAULT;
        if (!poll_once(key, ver, uc, &cont, &events, &pause)) {
            error_ = "чат не відповідає";
            break;
        }
        // Перша пачка — це історія чату; сипати нею в стрічку ні до чого.
        if (!first && sink_)
            for (const ChatEvent& ev : events) sink_(ev);
        first = false;
        if (!wait(pause)) break;
    }
    connected_ = false;
}

void YouTubeSource::stop() {
    {
        std::lock_guard<std::mutex> lock(mx_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    connected_ = false;
}

}  // namespace hominka
