// Розбір CSS — спільний для всіх систем. Навіщо окремо — у cssbits.h.
#include "gfx/cssbits.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hominka {

bool g_draw_trace = false;

void dtrace(const char* fmt, ...) {
    if (!g_draw_trace) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc(10, stderr);
    fflush(stderr);
}

// Читає один символ UTF-8: код і скільки байтів він зайняв.
unsigned utf8_cp(const std::string& s, size_t i, int* len) {
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
    return c;
}

// Найбільший радіус кута прямокутника. Direct2D малює скруглення одним
// радіусом на всі кути, CSS — чотирма. У темах чату вони завжди однакові
// (border-radius: 10px), тож беремо максимум і не ускладнюємо: розбіжність
// видно лише там, де кути навмисно різні.
float max_radius(const litehtml::border_radiuses& r) {
    float m = r.top_left_x;
    const float v[] = {r.top_left_y, r.top_right_x, r.top_right_y,
                       r.bottom_right_x, r.bottom_right_y,
                       r.bottom_left_x, r.bottom_left_y};
    for (float x : v) if (x > m) m = x;
    return m;
}

bool same_radius(const litehtml::border_radiuses& r) {
    const float a = r.top_left_x;
    const float v[] = {r.top_left_y, r.top_right_x, r.top_right_y,
                       r.bottom_right_x, r.bottom_right_y,
                       r.bottom_left_x, r.bottom_left_y};
    for (float x : v) if (std::fabs(x - a) > 0.5f) return false;
    return true;
}

// Розбір «rgba(r,g,b,a)» / «rgb(...)» / «#rgb» / «#rrggbb» — рівно те, що
// трапляється в text-shadow. Повний розбір кольорів робить сам litehtml; тут
// потрібен мінімум, бо цю властивість він не бачить.
// Одна тінь блока в розібраному вигляді.
bool parse_color(const std::string& s, Color* out) {
    if (s.empty()) return false;
    if (s[0] == '#') {
        unsigned v = 0;
        const std::string hex = s.substr(1);
        for (char c : hex) {
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            v = v * 16 + (unsigned)d;
        }
        if (hex.size() == 3) {
            const unsigned r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
            *out = rgba(r / 15.0f, g / 15.0f, b / 15.0f);
            return true;
        }
        if (hex.size() == 6) {
            *out = rgba(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f,
                                (v & 0xFF) / 255.0f, 1.0f);
            return true;
        }
        return false;
    }
    const size_t open = s.find('(');
    if (open == std::string::npos) return false;
    float p[4] = {0, 0, 0, 1};
    int n = 0;
    size_t i = open + 1;
    while (i < s.size() && n < 4) {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '/')) ++i;
        if (i >= s.size() || s[i] == ')') break;
        p[n++] = (float)atof(s.c_str() + i);
        while (i < s.size() && s[i] != ',' && s[i] != ')' && s[i] != ' ' && s[i] != '/') ++i;
    }
    if (n < 3) return false;
    *out = rgba(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, n >= 4 ? p[3] : 1.0f);
    return true;
}

// Розбирає значення text-shadow: «<dx> <dy> <розмиття> <колір>» у будь-якому
// порядку кольору й довжин. «none» — тіні немає.
bool parse_shadow_value(std::string val, TextShadow* out) {
    if (val.find("none") != std::string::npos) { out->on = false; return true; }

    // Колір може стояти як перед довжинами, так і після. Виймаємо його
    // окремо, решта — до трьох довжин у пікселях.
    std::string color_part;
    const size_t fn = val.find_first_of("#r");   // «#…» або «rgb…»
    if (fn != std::string::npos && (val[fn] == '#' || !val.compare(fn, 3, "rgb"))) {
        size_t stop = fn;
        if (val[fn] == '#') {
            stop = val.find_first_of(" \t", fn);
            if (stop == std::string::npos) stop = val.size();
        } else {
            stop = val.find(')', fn);
            stop = stop == std::string::npos ? val.size() : stop + 1;
        }
        color_part = val.substr(fn, stop - fn);
        val.erase(fn, stop - fn);
    }

    float nums[3] = {0, 0, 0};
    int n = 0;
    for (size_t i = 0; i < val.size() && n < 3; ) {
        if (isdigit((unsigned char)val[i]) || val[i] == '-' || val[i] == '.') {
            nums[n++] = (float)atof(val.c_str() + i);
            while (i < val.size() && (isdigit((unsigned char)val[i]) || val[i] == '.' ||
                                      val[i] == '-' || isalpha((unsigned char)val[i]))) ++i;
        } else {
            ++i;
        }
    }
    if (n == 0) return false;

    out->on = true;
    out->dx = nums[0];
    out->dy = n > 1 ? nums[1] : 0;
    out->blur = n > 2 ? nums[2] : 0;
    out->color = rgba(0, 0, 0, 1);
    if (!color_part.empty()) parse_color(color_part, &out->color);
    return true;
}

// Селектор правила, у якому оголошено властивість на позиції prop.
std::string rule_selector(const std::string& css, size_t prop) {
    // Початок правила — після попередньої «}» (або з початку файлу).
    size_t start = css.rfind('}', prop);
    start = start == std::string::npos ? 0 : start + 1;
    const size_t brace = css.rfind('{', prop);
    if (brace == std::string::npos || brace < start) return "";
    std::string sel = css.substr(start, brace - start);
    // Прибираємо переноси й зайві пробіли, щоб порівнювати було простіше.
    std::string out;
    for (char c : sel) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Значення властивості → один токен-ідентифікатор. Шістнадцятковий запис, бо
// в самому значенні є пробіли, коми й дужки («0 2px 3px rgba(0,0,0,.95)»), а
// токенізатор CSS мусить побачити щось одне ціле.
const char SHADOW_TAG[] = "hmkshadow";

std::string encode_shadow(const std::string& val) {
    static const char* HEX = "0123456789abcdef";
    std::string out = SHADOW_TAG;
    for (unsigned char c : val) { out += HEX[c >> 4]; out += HEX[c & 15]; }
    return out;
}

bool decode_shadow(const std::string& token, std::string* val) {
    const size_t tag = sizeof(SHADOW_TAG) - 1;
    if (token.size() <= tag || token.compare(0, tag, SHADOW_TAG) != 0) return false;
    val->clear();
    for (size_t i = tag; i + 1 < token.size(); i += 2) {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = hex(token[i]), lo = hex(token[i + 1]);
        if (hi < 0 || lo < 0) return false;
        *val += (char)(hi * 16 + lo);
    }
    return true;
}

// Одна тінь блока: «<dx> <dy> [розмиття] [розтяг] [колір] [inset]».
bool parse_box_shadow(const std::string& one, float font_size, BoxShadow* out) {
    std::string v = one;
    out->inset = v.find("inset") != std::string::npos;

    // Колір — окремо, решта суцільні числа.
    std::string color_part;
    const size_t fn = v.find_first_of("#r");
    if (fn != std::string::npos && (v[fn] == '#' || !v.compare(fn, 3, "rgb"))) {
        size_t stop;
        if (v[fn] == '#') {
            stop = v.find_first_of(" 	", fn);
            if (stop == std::string::npos) stop = v.size();
        } else {
            stop = v.find(')', fn);
            stop = stop == std::string::npos ? v.size() : stop + 1;
        }
        color_part = v.substr(fn, stop - fn);
        v.erase(fn, stop - fn);
    }

    float nums[4] = {0, 0, 0, 0};
    int n = 0;
    for (size_t i = 0; i < v.size() && n < 4; ) {
        if (isdigit((unsigned char)v[i]) || v[i] == '-' ||
            (v[i] == '.' && i + 1 < v.size() && isdigit((unsigned char)v[i + 1]))) {
            const float raw = (float)atof(v.c_str() + i);
            size_t j = i;
            while (j < v.size() && (isdigit((unsigned char)v[j]) || v[j] == '.' ||
                                    v[j] == '-')) ++j;
            // «em» рахується від кегля елемента — його нам і передали.
            const bool em = v.compare(j, 2, "em") == 0;
            nums[n++] = em ? raw * font_size : raw;
            while (j < v.size() && isalpha((unsigned char)v[j])) ++j;
            i = j;
        } else {
            ++i;
        }
    }
    if (n < 2) return false;          // без зсуву це не тінь

    out->dx = nums[0];
    out->dy = nums[1];
    out->blur = n > 2 ? nums[2] : 0.0f;
    out->spread = n > 3 ? nums[3] : 0.0f;
    out->color = rgba(0, 0, 0, 1);
    if (!color_part.empty()) parse_color(color_part, &out->color);
    return true;
}

// Кінець оголошення: перша «;» або «}» ЗА межами дужок і лапок.
//
// Наївний find_first_of(";}") тут не годиться, і помилка не лишається
// локальною: недорахована дужка з'їдає весь CSS, що йде далі, — тема втрачає
// не одне правило, а все нижче нього.
size_t decl_end(const std::string& s, size_t from) {
    int depth = 0;
    char quote = 0;
    for (size_t i = from; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(') ++depth;
        else if (c == ')') { if (depth > 0) --depth; }
        else if ((c == ';' || c == '}') && depth == 0) return i;
    }
    return s.size();
}

// Дужка, парна до s[open] == '('. Потрібна саме парна, а не перша-ліпша:
// «drop-shadow(0 1px 2px rgba(0, 0, 0, .5))» містить вкладену, і колір у
// вигляді rgba() — не рідкість, а типовий випадок.
size_t match_paren(const std::string& s, size_t open) {
    int depth = 0;
    char quote = 0;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(') ++depth;
        else if (c == ')' && --depth == 0) return i;
    }
    return std::string::npos;
}

std::string inject_shadow_channel(const std::string& css) {
    std::string out;
    out.reserve(css.size() + 256);
    size_t pos = 0;
    while (true) {
        const size_t at = css.find("text-shadow", pos);
        if (at == std::string::npos) { out.append(css, pos, std::string::npos); break; }
        // Саме властивість, а не збіг усередині іншого слова.
        const bool word_start = at == 0 || (!isalnum((unsigned char)css[at - 1]) &&
                                            css[at - 1] != '-' && css[at - 1] != '_');
        const size_t colon = css.find(':', at);
        if (!word_start || colon == std::string::npos) {
            out.append(css, pos, at + 11 - pos);
            pos = at + 11;
            continue;
        }
        const size_t end = decl_end(css, colon);

        out.append(css, pos, end - pos);          // саме оголошення лишаємо як є
        std::string val = css.substr(colon + 1, end - colon - 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\n')) val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\n')) val.pop_back();
        if (!val.empty()) {
            out += ";text-emphasis-style:";
            out += encode_shadow(val);
        }
        pos = end;
    }
    return out;
}

// «filter: drop-shadow(x y blur колір)» → «text-shadow: x y blur колір».
//
// Самого filter рушій не знає, і закладати його цілком означало б тягнути в
// чат окремий конвеєр ефектів. Але drop-shadow — це рівно те, що вже вміє наша
// тінь тексту, і саме його найчастіше приносять із чужих тем (в OBS-темах
// тінь пишуть то так, то так). Тож перекладаємо один цей випадок; решта
// filter, як і раніше, мовчки ігнорується.
std::string translate_drop_shadow(const std::string& css) {
    static const char kProp[] = "filter";
    static const char kFn[] = "drop-shadow(";
    std::string out;
    out.reserve(css.size() + 64);
    size_t pos = 0;
    while (true) {
        const size_t at = css.find(kProp, pos);
        if (at == std::string::npos) { out.append(css, pos, std::string::npos); break; }
        // Саме властивість «filter», а не «backdrop-filter» і не частина слова.
        const bool word_start = at == 0 || (!isalnum((unsigned char)css[at - 1]) &&
                                            css[at - 1] != '-' && css[at - 1] != '_');
        const size_t colon = css.find(':', at);
        if (!word_start || colon == std::string::npos) {
            out.append(css, pos, at + 6 - pos);
            pos = at + 6;
            continue;
        }
        const size_t end = decl_end(css, colon);
        const std::string val = css.substr(colon + 1, end - colon - 1);

        const size_t fn = val.find(kFn);
        // Саме парна дужка: перша-ліпша закрила б rgba() всередині, і далі в
        // CSS лишилася б відкрита дужка — вона проковтнула б усі наступні
        // правила теми.
        const size_t close = fn == std::string::npos
                                 ? std::string::npos
                                 : match_paren(val, fn + sizeof(kFn) - 2);
        if (fn == std::string::npos || close == std::string::npos) {
            out.append(css, pos, end - pos);          // не drop-shadow — лишаємо як є
            pos = end;
            continue;
        }
        // Саме оголошення лишаємо (нехай ігнорується), а поруч дописуємо тінь.
        out.append(css, pos, end - pos);
        out += ";text-shadow:";
        out += val.substr(fn + sizeof(kFn) - 1, close - fn - (sizeof(kFn) - 1));
        pos = end;
    }
    return out;
}

// Кегль: множимо ПІКСЕЛІ в розмірах шрифта.
//
// Чому не досить «типового розміру шрифта» в контейнері, який ми й так
// виставляли. Тому що і базова тема, і будь-яка чужа задає розмір абсолютним
// числом — body { font: 20px/1.35 … }, — і типовий розмір у такому документі
// не бере участі взагалі. Саме через це «A+» і «A−» не робили нічого: число
// доходило до стрічки, доходило до контейнера, і там його перебивав перший же
// рядок CSS. Видно це було й на самоперевірці: полотно з п'яти повідомлень
// мало ту саму висоту при 0.5, 1.0 і 2.0.
//
// Чіпаємо лише font, font-size і line-height: підпис під кнопками каже
// «Текст», а не «усе вікно», тож поля й скруглення лишаються як задумав автор
// теми. Відносні одиниці (em, %) масштабуються самі — вони рахуються від
// батька, а корінь ми вже помножили.
std::string scale_font_px(const std::string& css, float zoom) {
    if (zoom > 0.999f && zoom < 1.001f) return css;

    static const char* kProps[] = {"font-size", "line-height", "font"};
    std::string out;
    out.reserve(css.size() + 64);

    size_t pos = 0;
    while (pos < css.size()) {
        // Найближче з трьох оголошень попереду.
        size_t at = std::string::npos;
        size_t len = 0;
        for (const char* p : kProps) {
            const size_t n = strlen(p);
            size_t i = pos;
            for (;;) {
                i = css.find(p, i);
                if (i == std::string::npos) break;
                // Саме властивість, а не хвіст іншого слова («font» у
                // «font-family») і не частина назви.
                const bool left = i == 0 || (!isalnum((unsigned char)css[i - 1]) &&
                                             css[i - 1] != '-' && css[i - 1] != '_');
                const char after = i + n < css.size() ? css[i + n] : '\0';
                const bool right = after == ':' || after == ' ' || after == '\t';
                if (left && right) break;
                i += n;
            }
            if (i != std::string::npos && i < at) { at = i; len = n; }
        }
        if (at == std::string::npos) { out.append(css, pos, std::string::npos); break; }

        const size_t colon = css.find(':', at + len);
        if (colon == std::string::npos) { out.append(css, pos, std::string::npos); break; }
        const size_t end = decl_end(css, colon);

        out.append(css, pos, colon + 1 - pos);

        // Значення: множимо кожне число з «px», решту лишаємо як є. Лапки
        // пропускаємо цілком — назва шрифта може містити що завгодно.
        const std::string val = css.substr(colon + 1, end - colon - 1);
        size_t i = 0;
        while (i < val.size()) {
            const char c = val[i];
            if (c == '"' || c == '\'') {
                const size_t q = val.find(c, i + 1);
                const size_t stop = q == std::string::npos ? val.size() : q + 1;
                out.append(val, i, stop - i);
                i = stop;
                continue;
            }
            if (!isdigit((unsigned char)c) && !(c == '.' && i + 1 < val.size() &&
                                                isdigit((unsigned char)val[i + 1]))) {
                out += c;
                ++i;
                continue;
            }
            size_t j = i;
            while (j < val.size() && (isdigit((unsigned char)val[j]) || val[j] == '.')) ++j;
            if (val.compare(j, 2, "px") == 0 &&
                (j + 2 >= val.size() || !isalnum((unsigned char)val[j + 2]))) {
                char buf[32];
                snprintf(buf, sizeof buf, "%.3fpx", atof(val.substr(i, j - i).c_str()) * zoom);
                out += buf;
                i = j + 2;
            } else {
                out.append(val, i, j - i);
                i = j;
            }
        }
        pos = end;
    }
    return out;
}

std::string adapt_css(const std::string& css, float zoom) {
    // Два перетворення, і обидва — про те, чого litehtml не знає:
    //   * filter: drop-shadow(…) стає звичайною тінню тексту;
    //   * text-shadow (і своя, і щойно зроблена) їде каналом до контейнера.
    // Порядок значущий: спершу зробити тінь, потім її провести.
    //
    // «vertical-align: <довжина>» тут колись теж перекладалася (у
    // position:relative), але тепер її розуміє сам litehtml — див. латку в
    // native/patches/.
    return inject_shadow_channel(translate_drop_shadow(scale_font_px(css, zoom)));
}

TextShadow parse_text_shadow(const std::string& css) {
    // Тінь ми вміємо лише ОДНУ на весь рядок: litehtml про text-shadow не знає
    // взагалі, і різних тіней для різних частин рядка нам звідти не дістати.
    // Тому беремо ту, що оголошена для всієї сторінки — у правилі body (саме
    // так вона й задана в базових стилях чату). Правила на кшталт
    // «.b { text-shadow: none }» через це не діють: плашка отримає ту саму
    // тінь, що й решта рядка. Це помітно хіба що впритул.
    //
    // Якщо правила для body немає (чужа тема могла написати інакше) — беремо
    // останню оголошену в файлі: як і в браузері, виграє те, що нижче.
    TextShadow ts, body_shadow, last_shadow;
    bool have_body = false, have_last = false;

    size_t pos = 0;
    while ((pos = css.find("text-shadow", pos)) != std::string::npos) {
        // Береться саме властивість, а не збіг усередині слова чи рядка.
        const bool word_start = pos == 0 || (!isalnum((unsigned char)css[pos - 1]) &&
                                             css[pos - 1] != '-' && css[pos - 1] != '_');
        if (!word_start) { pos += 11; continue; }

        const size_t colon = css.find(':', pos);
        if (colon == std::string::npos) break;
        size_t end = css.find_first_of(";}", colon);
        if (end == std::string::npos) end = css.size();

        TextShadow parsed;
        if (parse_shadow_value(css.substr(colon + 1, end - colon - 1), &parsed)) {
            last_shadow = parsed;
            have_last = true;
            const std::string sel = rule_selector(css, pos);
            // «body», «html, body», «body, .m» — усе це правило для сторінки.
            if (sel == "body" || sel.find("body") != std::string::npos) {
                body_shadow = parsed;
                have_body = true;
            }
        }
        pos = end;
    }

    if (have_body) return body_shadow;
    if (have_last) return last_shadow;
    return ts;
}

// Тінь із токена, який приїхав каналом text-emphasis-style (див. вище).
bool shadow_from_channel(const std::string& token, TextShadow* out) {
    std::string val;
    if (!decode_shadow(token, &val)) return false;
    return parse_shadow_value(val, out);
}

std::vector<std::string> split_shadow_list(const std::string& value) {
    // Коми всередині rgba() не рахуються — інакше «rgba(0,0,0,.5)» розлетілося
    // б на чотири «тіні».
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
        if (i < value.size()) {
            if (value[i] == '(') ++depth;
            else if (value[i] == ')') --depth;
        }
        if (i == value.size() || (value[i] == ',' && depth == 0)) {
            parts.push_back(value.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

// --- перетворення ---------------------------------------------------------
//
// Матриця [a b c d e f] означає: x' = a*x + c*y + e, y' = b*x + d*y + f. Саме
// цей порядок і в Direct2D (_11.._32), і в Blend2D (m00..m21) — тож обидва
// приймають наш масив як є, без перекладання.

namespace {

void mat_identity(float m[6]) {
    m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 1; m[4] = 0; m[5] = 0;
}

// «спершу A, потім B».
void mat_mul(const float a[6], const float b[6], float out[6]) {
    float r[6];
    r[0] = a[0] * b[0] + a[1] * b[2];
    r[1] = a[0] * b[1] + a[1] * b[3];
    r[2] = a[2] * b[0] + a[3] * b[2];
    r[3] = a[2] * b[1] + a[3] * b[3];
    r[4] = a[4] * b[0] + a[5] * b[2] + b[4];
    r[5] = a[4] * b[1] + a[5] * b[3] + b[5];
    for (int i = 0; i < 6; ++i) out[i] = r[i];
}

}  // namespace

bool parse_transform(const std::string& value, float font_size,
                     const litehtml::position& border_box, float m[6]) {
    if (value.empty()) return false;

    // Типовий transform-origin — центр елемента: крутимо й масштабуємо навколо
    // нього, а не навколо кута полотна.
    const float cx = border_box.x + border_box.width / 2.0f;
    const float cy = border_box.y + border_box.height / 2.0f;

    mat_identity(m);
    bool any = false;

    size_t i = 0;
    while (i < value.size()) {
        const size_t open = value.find('(', i);
        if (open == std::string::npos) break;
        const size_t close = value.find(')', open);
        if (close == std::string::npos) break;

        std::string name = value.substr(i, open - i);
        while (!name.empty() && !isalpha((unsigned char)name.front())) name.erase(name.begin());
        for (char& c : name) c = (char)tolower((unsigned char)c);

        const std::string args = value.substr(open + 1, close - open - 1);
        float n[2] = {0, 0};
        int cnt = 0;
        for (size_t k = 0; k < args.size() && cnt < 2; ) {
            if (isdigit((unsigned char)args[k]) || args[k] == '-' || args[k] == '.') {
                const float raw = (float)atof(args.c_str() + k);
                size_t j = k;
                while (j < args.size() && (isdigit((unsigned char)args[j]) ||
                                           args[j] == '.' || args[j] == '-')) ++j;
                const bool em = args.compare(j, 2, "em") == 0;
                n[cnt++] = em ? raw * font_size : raw;
                while (j < args.size() && isalpha((unsigned char)args[j])) ++j;
                k = j;
            } else {
                ++k;
            }
        }

        float t[6];
        bool got = true;
        if (name == "translate") {
            mat_identity(t); t[4] = n[0]; t[5] = cnt > 1 ? n[1] : 0.0f;
        } else if (name == "translatex") {
            mat_identity(t); t[4] = n[0];
        } else if (name == "translatey") {
            mat_identity(t); t[5] = n[0];
        } else if (name == "scale" || name == "scalex" || name == "scaley") {
            const float sx = name == "scaley" ? 1.0f : n[0];
            const float sy = name == "scalex" ? 1.0f
                                              : (name == "scale" && cnt > 1 ? n[1] : n[0]);
            t[0] = sx; t[1] = 0; t[2] = 0; t[3] = sy;
            t[4] = cx - sx * cx; t[5] = cy - sy * cy;
        } else if (name == "rotate") {
            const float rad = n[0] * 3.14159265358979f / 180.0f;
            const float co = cosf(rad), si = sinf(rad);
            t[0] = co; t[1] = si; t[2] = -si; t[3] = co;
            t[4] = cx - cx * co + cy * si;
            t[5] = cy - cx * si - cy * co;
        } else {
            // Решта (skew, matrix, тривимірні) — мовчки повз: краще без них,
            // ніж навмання.
            got = false;
        }
        if (got) { mat_mul(m, t, m); any = true; }

        i = close + 1;
    }
    return any;
}


// --- перенос рядка --------------------------------------------------------

void split_text_words(const char* text,
                      const std::function<void(const char*)>& on_word,
                      const std::function<void(const char*)>& on_space) {
    // Де рядок МОЖНА перенести. Типовий поділ у litehtml — лише пробіли (та
    // ієрогліфи), і через це довге посилання не переносилося ніде: воно
    // виїжджало за край вікна й обрізалося. Браузер у тому ж місці ріже його
    // на чотири рядки — перевірено на тому самому повідомленні.
    //
    // Ріжемо там, де ріже браузер: ПІСЛЯ роздільників усередині адрес. Кома й
    // крапка сюди не входять навмисно — інакше «3,14» і «19.00» розліталися б
    // на два рядки.
    static const char* kBreakAfter = "/?&=#-_";

    // Стеля на суцільний прогін без жодного роздільника. Такого в живій мові
    // не буває (найдовші українські слова — близько 25 літер), а от у
    // випадковому рядку з посилання чи в base64 буває легко. Без стелі такий
    // прогін однаково не мав би де перенестися.
    const int kMaxRun = 28;

    std::string word;
    int run = 0;                       // символів (не байтів) у поточному прогоні

    auto flush = [&]() {
        if (word.empty()) return;
        on_word(word.c_str());
        word.clear();
        run = 0;
    };

    const std::string s(text ? text : "");
    size_t i = 0;
    while (i < s.size()) {
        int len = 0;
        const unsigned cp = utf8_cp(s, i, &len);

        // Пробіли — як і було: слово закінчилося, проміжок окремо.
        if (cp == ' ' || cp == '\t' || cp == '\n' ||
            cp == '\r' || cp == '\f') {
            flush();
            on_space(s.substr(i, len).c_str());
            i += len;
            continue;
        }
        // Ієрогліфи переносяться посимвольно — так само, як у litehtml.
        if (cp >= 0x4E00 && cp <= 0x9FCC) {
            flush();
            on_word(s.substr(i, len).c_str());
            i += len;
            continue;
        }

        word.append(s, i, len);
        ++run;
        i += len;

        if (cp < 128 && strchr(kBreakAfter, (char)cp)) {
            flush();                   // перенести МОЖНА після роздільника
        } else if (run >= kMaxRun) {
            flush();                   // прогін без роздільників — ріжемо силою
        }
    }
    flush();
}

}  // namespace hominka
