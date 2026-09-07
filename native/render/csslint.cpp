#include "csslint.h"

#include <map>
#include <set>

namespace hominka {

namespace {

// Перелік складено за тим, що рушій СПРАВДІ розбирає, а не за здогадами:
// помилкова підказка гірша за жодну.
const std::vector<CssNote> kUnsupported = {
    {"filter",
     "не діє (крім filter: drop-shadow — його перекладаємо в text-shadow)",
     "тінь тексту — text-shadow, тінь рядка — box-shadow; решту ефектів "
     "доведеться закласти в самі кольори"},
    {"backdrop-filter",
     "не діє: матового скла не буде",
     "звичайна напівпрозорість: background: rgba(0,0,0,.45)"},
    {"animation",
     "не діє: рядок одразу в кінцевому стані",
     "поява рядка вже анімована самою програмою (зсув і проявлення); вимкнути "
     "її можна звичним «.m { animation: none }» — це програма розуміє"},
    {"transition", "не діє: змінюватися в оверлеї нема чому", ""},
    {"mask", "не діє: елемент намалюється повністю", ""},
    {"clip-path", "не діє: обрізання не буде", "скруглення кутів робить border-radius"},
    {"grid", "не діє: сітки немає",
     "рядок і так один; для розкладки всередині нього є flex"},
    {"letter-spacing", "не діє: відстань між літерами лишиться звичайною",
     "виділити нік чи назву можна жирністю (font-weight), кеглем або кольором"},
    {"word-break", "не діє", "довгі слова переносяться самі по пробілах"},
    {"text-overflow", "не діє: «…» замість обрізаного не буде", ""},
};

// Правила @, яких рушій не виконує.
const std::map<std::string, CssNote> kUnsupportedAt = {
    {"keyframes", {"keyframes", "не діє: кадрів анімації рушій не програє",
                   "поява рядка вже анімована самою програмою"}},
    {"media", {"media", "не діє: розмір вікна чату задає сама програма", ""}},
    {"supports", {"supports", "не діє", ""}},
    {"import", {"import", "не діє: сторінка чату навмисно самодостатня й у мережу не ходить",
                "вставте потрібні правила прямо сюди"}},
};

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

// Довгий уривок у повідомленні лише заважає читати саме повідомлення.
std::string shorten(const std::string& text, size_t limit = 40) {
    std::string one;
    bool space = false;
    for (char c : text) {
        if ((unsigned char)c <= ' ') { space = !one.empty(); continue; }
        if (space) { one += ' '; space = false; }
        one += c;
    }
    if (one.size() <= limit) return one;
    return one.substr(0, limit) + "…";
}

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_alnum(char c) { return is_alpha(c) || (c >= '0' && c <= '9'); }

void check_decl(const std::string& decl, int line, std::vector<CssProblem>* out) {
    const size_t colon = decl.find(':');
    if (colon == std::string::npos) {
        out->push_back({line, "оголошення без двокрапки: «" + shorten(decl) + "»"});
        return;
    }
    const std::string prop = trimmed(decl.substr(0, colon));
    const std::string value = trimmed(decl.substr(colon + 1));
    if (prop.empty()) out->push_back({line, "немає назви властивості перед двокрапкою"});
    else if (value.empty())
        out->push_back({line, "властивість «" + prop + "» без значення"});
}

int count_nl(const std::string& s, size_t from, size_t to) {
    int n = 0;
    for (size_t i = from; i < to && i < s.size(); ++i) if (s[i] == '\n') ++n;
    return n;
}

// Що знайшов прохід по тексту: властивість або @-правило.
struct Found {
    int line;
    bool at_rule;
    std::string name;
    std::string value;
};

// Проходить CSS і віддає імена властивостей і @-правил разом із номерами
// рядків. Не парсер: дерево нам ні до чого. Але коментарі й лапки пропускаємо
// як слід — інакше «/* opacity: 1 */» дало б попередження про закоментоване.
std::vector<Found> scan(const std::string& text) {
    std::vector<Found> out;
    size_t i = 0;
    int line = 1;
    const size_t n = text.size();

    while (i < n) {
        const char ch = text[i];
        if (ch == '\n') { ++line; ++i; continue; }
        if (ch == '/' && i + 1 < n && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            if (end == std::string::npos) return out;
            line += count_nl(text, i, end);
            i = end + 2;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            size_t end = i + 1;
            while (end < n && text[end] != ch) end += (text[end] == '\\') ? 2 : 1;
            line += count_nl(text, i, end + 1 < n ? end + 1 : n);
            i = end + 1;
            continue;
        }
        if (ch == '@') {
            size_t j = i + 1;
            while (j < n && (is_alpha(text[j]) || text[j] == '-')) ++j;
            out.push_back({line, true, lower(text.substr(i + 1, j - i - 1)), ""});
            i = j;
            continue;
        }
        if (ch == ':') {
            size_t j = i;
            while (j > 0 && (is_alnum(text[j - 1]) || text[j - 1] == '-' || text[j - 1] == '_'))
                --j;
            const std::string name = lower(trimmed(text.substr(j, i - j)));
            // Селектор «a:hover» теж має двокрапку — але перед ним не «{».
            const size_t brace = text.rfind('{', i);
            const size_t close = text.rfind('}', i);
            const bool inside = brace != std::string::npos &&
                                (close == std::string::npos || brace > close);
            if (!name.empty() && inside) {
                size_t end = i + 1;
                while (end < n && text[end] != ';' && text[end] != '}') ++end;
                out.push_back({line, false, name,
                               lower(trimmed(text.substr(i + 1, end - i - 1)))});
            }
            ++i;
            continue;
        }
        ++i;
    }
    return out;
}

}  // namespace

const std::vector<CssNote>& css_unsupported() { return kUnsupported; }

const CssNote* css_note(const std::string& prop) {
    std::string p = lower(trimmed(prop));
    // Префікси постачальників («-webkit-mask») поводяться так само.
    for (const char* pref : {"-webkit-", "-moz-", "-ms-", "-o-"}) {
        const size_t len = std::string(pref).size();
        if (p.compare(0, len, pref) == 0) { p = p.substr(len); break; }
    }
    for (const CssNote& n : kUnsupported) if (p == n.prop) return &n;
    // Складені імена: grid-template-columns, animation-name, mask-image…
    for (const CssNote& n : kUnsupported) {
        const std::string pre = std::string(n.prop) + "-";
        if (p.compare(0, pre.size(), pre) == 0) return &n;
    }
    return nullptr;
}

std::vector<CssProblem> validate_css(const std::string& text) {
    std::vector<CssProblem> errors;
    int depth = 0;
    std::vector<int> open_lines;
    size_t i = 0;
    int line = 1;
    const size_t n = text.size();
    int decl_start_line = 1;      // рядок, де почалося поточне оголошення
    std::string buf;              // накопичене оголошення (між «;» та «}»)
    bool at_rule = false;

    while (i < n) {
        const char ch = text[i];
        if (ch == '\n') { ++line; ++i; continue; }

        if (ch == '/' && i + 1 < n && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            if (end == std::string::npos) {
                errors.push_back({line, "коментар /* не закрито"});
                break;
            }
            line += count_nl(text, i, end);
            i = end + 2;
            continue;
        }

        if (ch == '"' || ch == '\'') {
            const size_t start = i;
            size_t end = i + 1;
            while (end < n && text[end] != ch) {
                if (text[end] == '\\') ++end;
                else if (text[end] == '\n') break;
                ++end;
            }
            if (end >= n || text[end] != ch) {
                errors.push_back({line, std::string("лапки ") + ch + " не закрито"});
                break;
            }
            // Лапковий рядок — це ЗНАЧЕННЯ (напр. content: ':'), тож він іде в
            // буфер оголошення. Без цього значення губилося, і «content: ':'»
            // виглядало як властивість без значення.
            if (buf.empty() && !trimmed(text.substr(start, end - start + 1)).empty())
                decl_start_line = line;
            buf += text.substr(start, end - start + 1);
            i = end + 1;
            continue;
        }

        if (ch == '{') {
            const std::string selector = trimmed(buf);
            if (depth == 0 && selector.empty())
                errors.push_back({line, "порожній селектор перед {"});
            at_rule = !selector.empty() && selector[0] == '@';
            ++depth;
            open_lines.push_back(line);
            buf.clear();
            decl_start_line = line;
            ++i;
            continue;
        }

        if (ch == '}') {
            std::string rest = trimmed(buf);
            while (!rest.empty() && rest.back() == ';') rest.pop_back();
            rest = trimmed(rest);
            if (!rest.empty() && depth > 0 && !at_rule)
                check_decl(rest, decl_start_line, &errors);
            if (depth == 0) errors.push_back({line, "зайва } — блок не було відкрито"});
            else {
                --depth;
                open_lines.pop_back();
            }
            buf.clear();
            decl_start_line = line;
            ++i;
            continue;
        }

        if (ch == ';') {
            const std::string decl = trimmed(buf);
            if (depth > 0 && !decl.empty()) check_decl(decl, decl_start_line, &errors);
            buf.clear();
            decl_start_line = line;
            ++i;
            continue;
        }

        if (buf.empty() && (unsigned char)ch > ' ') decl_start_line = line;
        buf += ch;
        ++i;
    }

    if (depth > 0)
        errors.push_back({open_lines.empty() ? line : open_lines.back(), "блок { не закрито"});
    const std::string tail = trimmed(buf);
    if (depth == 0 && !tail.empty() && tail[0] != '@')
        errors.push_back({decl_start_line, "текст поза правилом: «" + shorten(tail) + "»"});
    return errors;
}

std::vector<CssProblem> lint_css(const std::string& text) {
    std::vector<CssProblem> warns;
    std::set<std::string> seen;   // про ту саму властивість — один раз

    for (const Found& f : scan(text)) {
        if (f.at_rule) {
            auto hit = kUnsupportedAt.find(f.name);
            if (hit == kUnsupportedAt.end() || seen.count("@" + f.name)) continue;
            seen.insert("@" + f.name);
            std::string msg = "@" + f.name + " " + hit->second.what;
            if (hit->second.instead && *hit->second.instead)
                msg += ". Замість: " + std::string(hit->second.instead);
            warns.push_back({f.line, msg});
            continue;
        }

        // «animation: none» програма виконує сама (вимикає появу рядка), тож
        // це не той випадок, коли правило нічого не робить.
        if (f.name == "animation" && f.value == "none") continue;
        // «filter: drop-shadow(...)» ми перекладаємо в тінь тексту — працює.
        if (f.name == "filter" && f.value.compare(0, 12, "drop-shadow(") == 0) continue;

        if (f.name == "display" && (f.value == "grid" || f.value == "inline-grid")) {
            if (seen.count("display:grid")) continue;
            seen.insert("display:grid");
            const CssNote* n = css_note("grid");
            if (n)
                warns.push_back({f.line, "«display: " + f.value + "» " + n->what +
                                             ". Замість: " + n->instead});
            continue;
        }

        const CssNote* n = css_note(f.name);
        if (!n || seen.count(f.name)) continue;
        seen.insert(f.name);
        std::string msg = "«" + f.name + "» " + n->what;
        if (n->instead && *n->instead) msg += ". Замість: " + std::string(n->instead);
        warns.push_back({f.line, msg});
    }
    return warns;
}

}  // namespace hominka
