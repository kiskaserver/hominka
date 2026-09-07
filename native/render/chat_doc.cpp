#include "chat_doc.h"

#include <cwctype>
#include <cstring>

#include "cssbits.h"
#include "page_assets.h"

namespace hominka {

namespace {

// --- UTF-8 -----------------------------------------------------------------
// Свій розбір, а не std::regex: правило згадки написане через \p{L}\p{N} і
// look-behind, чого std::regex не вміє взагалі. А правило має бути тим самим,
// що на сторінці й на сайті, — інакше підсвітка «@ніка» розійдеться.

// Читає один символ. Повертає його код і скільки байтів він зайняв.
unsigned utf8_next(const std::string& s, size_t i, int* len) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { *len = 1; return c; }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        *len = 2; return ((c & 0x1Fu) << 6) | ((unsigned char)s[i + 1] & 0x3Fu);
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        *len = 3;
        return ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) |
               ((unsigned char)s[i + 2] & 0x3Fu);
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        *len = 4;
        return ((c & 0x07u) << 18) | (((unsigned char)s[i + 1] & 0x3Fu) << 12) |
               (((unsigned char)s[i + 2] & 0x3Fu) << 6) | ((unsigned char)s[i + 3] & 0x3Fu);
    }
    *len = 1;
    return c;                                   // побитий байт — беремо як є
}

// Початок символу, що передує позиції i.
size_t utf8_prev(const std::string& s, size_t i) {
    if (i == 0) return 0;
    size_t j = i - 1;
    while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) --j;
    return j;
}

// \p{L} або \p{N} — літера чи цифра будь-якої абетки. Поза BMP літер, з яких
// складають ніки, не буває, тож усе, що вище, вважаємо не-літерою.
bool is_alnum_cp(unsigned cp) {
    if (cp > 0xFFFF) return false;
    return iswalnum((wint_t)cp) != 0;
}

const char* const PUNCT_TAIL = ".,!?:;)]}";     // плюс «…» і «»» — багатобайтні

// Чи є символ на позиції i «хвостовою» пунктуацією. Набір той самий, що в
// PUNCT на сторінці: «Kappa!» — це емоут Kappa плюс «!».
bool is_punct_at(const std::string& s, size_t i, int* len) {
    unsigned cp = utf8_next(s, i, len);
    if (cp < 128 && strchr(PUNCT_TAIL, (char)cp)) return true;
    return cp == 0x2026 /* … */ || cp == 0x00BB /* » */;
}

bool is_space_cp(unsigned cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x00A0;
}

// --- дрібне ----------------------------------------------------------------

std::string b64(const uint8_t* d, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const unsigned a = d[i];
        const unsigned b = i + 1 < n ? d[i + 1] : 0;
        const unsigned c = i + 2 < n ? d[i + 2] : 0;
        const unsigned v = (a << 16) | (b << 8) | c;
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += i + 1 < n ? T[(v >> 6) & 63] : '=';
        out += i + 2 < n ? T[v & 63] : '=';
    }
    return out;
}

const char* platform_icon_svg(const std::string& platform) {
    for (const auto& p : PLATFORM_ICONS)
        if (platform == p.id) return p.svg;
    return nullptr;
}

const BadgeLabel* badge_label(const std::string& id) {
    for (const auto& b : BADGE_LABELS)
        if (id == b.id) return &b;
    return nullptr;
}

// Колір ніка: приймаємо лише «#» плюс 3–8 шістнадцяткових. Так само на
// сторінці — інакше довільний рядок із мережі потрапив би просто в style.
std::string nick_color(const std::string& c) {
    if (c.size() >= 4 && c.size() <= 9 && c[0] == '#') {
        bool ok = true;
        for (size_t i = 1; i < c.size(); ++i)
            if (!isxdigit((unsigned char)c[i])) { ok = false; break; }
        if (ok) return c;
    }
    return "#f87171";
}

bool is_system(const ChatMessage& m) { return m.kind == "system"; }

}  // namespace

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

namespace {

// Екранований текст із підсвіченими згадками. Порт textPart() + MENTION.
std::string text_part(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '@') {
            int len = 0;
            utf8_next(s, i, &len);
            out += html_escape(s.substr(i, len));
            i += len;
            continue;
        }
        // Look-behind: перед «@» не має бути літери, цифри, «_», «@», «.», «-».
        bool ok_before = true;
        if (i > 0) {
            const size_t p = utf8_prev(s, i);
            int plen = 0;
            const unsigned pc = utf8_next(s, p, &plen);
            if (is_alnum_cp(pc) || pc == '_' || pc == '@' || pc == '.' || pc == '-')
                ok_before = false;
        }
        if (!ok_before) { out += '@'; ++i; continue; }

        // Тіло згадки: до 31 символу з [літера цифра _ . -], і закінчитися
        // воно мусить на [літера цифра _].
        size_t j = i + 1;
        size_t last_good_end = 0;                // кінець найдовшого дійсного збігу
        int taken = 0;
        while (j < s.size() && taken < 32) {
            int len = 0;
            const unsigned cp = utf8_next(s, j, &len);
            const bool body = is_alnum_cp(cp) || cp == '_' || cp == '.' || cp == '-';
            if (!body) break;
            j += len;
            ++taken;
            if (is_alnum_cp(cp) || cp == '_') last_good_end = j;
        }
        if (last_good_end > i + 1) {
            out += "<span class=\"at\">";
            out += html_escape(s.substr(i, last_good_end - i));
            out += "</span>";
            i = last_good_end;
        } else {
            out += '@';
            ++i;
        }
    }
    return out;
}

std::string emote_img(const std::string& url, const std::string& code) {
    return "<img class=\"em\" src=\"" + html_escape(url) + "\" alt=\"" +
           html_escape(code) + "\">";
}

// Тіло повідомлення: текст плюс емоути. Порт body().
//
// Емоут — ОКРЕМЕ слово: пробіл ліворуч дає сам поділ, праворуч допускаємо
// хвіст пунктуації. Так «Kappa!» лишається емоутом, а короткий код більше не
// спалахує всередині довшого слова («cat» у «category»).
std::string message_body(const std::string& text, const std::vector<Emote>& emotes,
                         const ImageReady& ready) {
    if (emotes.empty()) return text_part(text);

    // Емоут, картинки якого ще немає, лишається звичайним словом («Kappa») —
    // саме так чат виглядав, доки емоутів не було взагалі. Дірки в рядку не
    // буває, а коли картинка приїде, наступний кадр намалює вже її.
    auto url_for = [&](const std::string& code) -> const std::string* {
        for (const auto& e : emotes) {
            if (e.code.empty() || e.code != code) continue;
            if (ready && !ready(e.url)) return nullptr;
            return &e.url;
        }
        return nullptr;
    };

    std::string out;
    out.reserve(text.size() + 64);
    size_t i = 0;
    while (i < text.size()) {
        // Проміжки віддаємо як є — вони теж частина розкладки рядка.
        int len = 0;
        unsigned cp = utf8_next(text, i, &len);
        if (is_space_cp(cp)) { out += html_escape(text.substr(i, len)); i += len; continue; }

        size_t start = i;
        while (i < text.size()) {
            int l = 0;
            const unsigned c = utf8_next(text, i, &l);
            if (is_space_cp(c)) break;
            i += l;
        }
        const std::string token = text.substr(start, i - start);

        if (const std::string* u = url_for(token)) { out += emote_img(*u, token); continue; }

        // Відриваємо хвіст пунктуації і пробуємо ще раз.
        size_t b = token.size();
        while (b > 0) {
            const size_t p = utf8_prev(token, b);
            int plen = 0;
            if (!is_punct_at(token, p, &plen)) break;
            b = p;
        }
        const std::string core = token.substr(0, b);
        if (b < token.size() && !core.empty()) {
            if (const std::string* u = url_for(core)) {
                out += emote_img(*u, core);
                out += text_part(token.substr(b));
                continue;
            }
        }
        out += text_part(token);
    }
    return out;
}

// --- частини рядка ---------------------------------------------------------
// Порт PARTS зі сторінки: кожна вміє намалювати себе сама, порядок задає
// список. Саме тому «Порядок» у редакторі — це переставляння елементів, а не
// правка коду.

std::string part_ico(const ChatMessage& m) {
    const char* svg = platform_icon_svg(m.platform);
    if (!svg) return "";
    // <img> замість вбудованого <svg>: litehtml малює SVG як картинку, але не
    // як елемент розмітки. Клас «.ico» — той самий, він і описаний у довіднику.
    return "<img class=\"ico\" src=\"data:image/svg+xml;base64," +
           b64((const uint8_t*)svg, strlen(svg)) + "\" alt=\"" +
           html_escape(m.platform) + "\">";
}

std::string part_badges(const ChatMessage& m, const ImageReady& ready) {
    // У системної події немає ні автора, ні плашок — без цієї перевірки на
    // місці ніка малювалося б «undefined:».
    if (is_system(m)) return "";
    std::string out;
    for (const auto& b : m.badges) {
        // Є справжня іконка — малюємо її; немає — текстову плашку.
        const std::string* icon = nullptr;
        for (const auto& bi : m.badge_icons) {
            if (bi.id != b || bi.url.empty()) continue;
            if (ready && !ready(bi.url)) break;      // ще нема — далі буде плашка
            icon = &bi.url;
            break;
        }
        if (icon) {
            out += "<img class=\"bi\" src=\"" + html_escape(*icon) + "\" alt=\"" +
                   html_escape(b) + "\" title=\"" + html_escape(b) + "\">";
            continue;
        }
        if (const BadgeLabel* lb = badge_label(b)) {
            out += "<span class=\"b\" style=\"background:";
            out += lb->bg;
            out += ";color:";
            out += lb->fg;
            out += "\">";
            out += lb->text;
            out += "</span>";
        }
    }
    return out;
}

std::string part_reply(const ChatMessage& m) {
    if (is_system(m) || m.reply.empty()) return "";
    return "<span class=\"re\">\xE2\x86\xB3 " + html_escape(m.reply) + "</span>";
}

std::string part_name(const ChatMessage& m) {
    if (is_system(m)) return "";
    return "<span class=\"n\" style=\"color:" + nick_color(m.color) + "\">" +
           html_escape(m.name) + "</span>";
}

std::string part_money(const ChatMessage& m) {
    if (m.amount.empty()) return "";
    return "<span class=\"money\">" + html_escape(m.amount) + "</span>";
}

std::string part_text(const ChatMessage& m, const ImageReady& ready) {
    // Системні події займають те саме місце, що й текст повідомлення: місце в
    // рядку одне, а вигляд у них різний.
    if (is_system(m))
        return "<span class=\"sys\">" + html_escape(m.text) + "</span>";
    return "<span class=\"t\">" + message_body(m.text, m.emotes, ready) + "</span>";
}

std::string render_part(const std::string& id, const ChatMessage& m,
                        const ImageReady& ready) {
    if (id == "ico")    return part_ico(m);
    if (id == "badges") return part_badges(m, ready);
    if (id == "reply")  return part_reply(m);
    if (id == "name")   return part_name(m);
    if (id == "money")  return part_money(m);
    if (id == "text")   return part_text(m, ready);
    return "";
}

}  // namespace

std::vector<std::string> clean_layout(const std::vector<std::string>& layout) {
    std::vector<std::string> out;
    std::vector<std::string> seen;
    for (const std::string& raw : layout) {
        const std::string pid = (!raw.empty() && raw[0] == '-') ? raw.substr(1) : raw;
        bool known = false;
        for (const char* p : PART_IDS) if (pid == p) { known = true; break; }
        if (!known) continue;
        bool dup = false;
        for (const auto& s : seen) if (s == pid) { dup = true; break; }
        if (dup) continue;
        out.push_back(raw);
        seen.push_back(pid);
    }
    if (out.empty()) {
        for (const char* p : PART_IDS) out.push_back(p);
        return out;
    }
    // Частини, яких у збереженому порядку немає, дописуємо в кінець: так нова
    // частина з'являється у всіх, а не лише в тих, хто ще не чіпав налаштування.
    for (const char* p : PART_IDS) {
        bool found = false;
        for (const auto& s : seen) if (s == p) { found = true; break; }
        if (!found) out.push_back(p);
    }
    return out;
}

// Рядкове поле JSON. Числа й булеві теж зводимо до рядка: Python могла
// прислати «amount: 200», і це так само сума.
std::string get_str(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

ChatMessage message_from_json(const nlohmann::json& j) {
    ChatMessage m;
    m.id = get_str(j, "id");
    m.platform = get_str(j, "platform");
    m.kind = get_str(j, "kind");
    m.event = get_str(j, "event");
    m.nick = get_str(j, "nick");
    m.name = get_str(j, "name");
    m.color = get_str(j, "color");
    m.text = get_str(j, "text");
    m.reply = get_str(j, "reply");
    m.amount = get_str(j, "amount");

    auto badges = j.find("badges");
    if (badges != j.end() && badges->is_array())
        for (const auto& b : *badges)
            if (b.is_string()) m.badges.push_back(b.get<std::string>());

    auto icons = j.find("badgeIcons");
    if (icons != j.end() && icons->is_array())
        for (const auto& b : *icons)
            if (b.is_object()) m.badge_icons.push_back({get_str(b, "id"), get_str(b, "url")});

    auto emotes = j.find("emotes");
    if (emotes != j.end() && emotes->is_array())
        for (const auto& e : *emotes)
            if (e.is_object()) m.emotes.push_back({get_str(e, "code"), get_str(e, "url")});
    return m;
}

std::string message_html(const ChatMessage& m, const std::vector<std::string>& layout,
                         const ImageReady& ready) {
    std::string cls = "m";
    if (!m.amount.empty()) cls += " paid";

    std::string out = "<div class=\"" + cls + "\"";
    if (!m.id.empty())   out += " data-id=\"" + html_escape(m.id) + "\"";
    if (!m.nick.empty()) out += " data-nick=\"" + html_escape(m.nick) + "\"";
    // Площадка й вид повідомлення — атрибутами: по них пишеться свій CSS
    // («.m[data-platform="twitch"]»), і це єдиний спосіб відрізнити Twitch від
    // Kick, не розбираючи вміст рядка.
    out += " data-platform=\"" + html_escape(m.platform.empty() ? "site" : m.platform) + "\"";
    out += " data-kind=\"";
    out += is_system(m) ? "system" : (m.amount.empty() ? "message" : "money");
    out += "\"";
    if (!m.event.empty()) out += " data-event=\"" + html_escape(m.event) + "\"";
    out += ">";

    for (const std::string& id : clean_layout(layout)) {
        if (!id.empty() && id[0] == '-') continue;        // вимкнена частина
        out += render_part(id, m, ready);
    }
    out += "</div>";
    return out;
}

std::string message_document(const ChatMessage& m, const std::vector<std::string>& layout,
                             const std::string& user_css, const ImageReady& ready) {
    // Свій CSS іде ОКРЕМИМ тегом і НИЖЧЕ базового: так будь-яке правило
    // перебиває типове без !important, а «скинути до типових» — це просто
    // спорожнити цей тег.
    //
    // Екрануємо лише «</style»: усе інше в CSS нешкідливе, а от закритий тег
    // усередині стилю вирвався б у розмітку й зробив із CSS довільний HTML.
    std::string safe = user_css;
    for (size_t p = safe.find("</style"); p != std::string::npos;
         p = safe.find("</style", p + 8))
        safe.replace(p, 7, "<\\/style");

    // Стилі перед видачею litehtml пристосовуємо: тінь проводимо окремим
    // каналом, а vertical-align із довжиною перекладаємо у відносне зміщення —
    // ні того, ні того litehtml не знає (див. adapt_css).
    std::string out = "<!doctype html><html><head><meta charset=\"utf-8\"><style>";
    out += adapt_css(BASE_CSS);
    out += "</style><style id=\"userCss\">";
    out += adapt_css(safe);
    out += "</style></head><body>";
    out += message_html(m, layout, ready);
    out += "</body></html>";
    return out;
}

}  // namespace hominka
