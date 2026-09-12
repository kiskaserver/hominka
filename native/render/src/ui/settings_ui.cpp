#include "ui/settings_ui.h"

#include <cstdio>
#include <cstring>

#include "core/version.h"
#include "imgui/imgui.h"
#include "ui/uibits.h"

namespace hominka {

namespace {

// Ті самі кольори, що були у Qt-панелі (hominka/styles.py). Люди впізнають
// програму по них, і міняти їх лише тому, що змінився рушій, — привід
// пояснювати старим користувачам, що нічого не зламалося.
const ImU32 ACCENT     = IM_COL32(168, 85, 247, 255);   // #a855f7
const ImU32 ACCENT_DIM = IM_COL32(196, 181, 253, 255);  // #c4b5fd
const ImU32 TEXT       = IM_COL32(228, 228, 231, 255);
const ImU32 TEXT_DIM   = IM_COL32(139, 139, 147, 255);
const ImU32 TEXT_FIELD = IM_COL32(161, 161, 170, 255);
const ImU32 TITLE_BG   = IM_COL32(23, 20, 31, 255);
const ImU32 RAIL_BG    = IM_COL32(18, 17, 23, 255);
const ImU32 LINE       = IM_COL32(255, 255, 255, 16);

// Стан джерела кольором. Зеленого й сірого досить у 90% випадків; жовтий —
// «ще не під'єдналося», червоний — «сказало, що не вийде».
const ImU32 DOT_LIVE   = IM_COL32(34, 197, 94, 255);
const ImU32 DOT_WAIT   = IM_COL32(251, 191, 36, 255);
const ImU32 DOT_ERR    = IM_COL32(248, 113, 113, 255);
const ImU32 DOT_OFF    = IM_COL32(90, 90, 100, 255);

const float TITLE_H = 42.0f;
const float RAIL_W = 178.0f;
const float STATUS_H = 30.0f;
const float PAD = 18.0f;
const float LABEL_W = 100.0f;     // стовпчик підписів у розділі «Канали»

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

void text_col(ImU32 c, const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(c));
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}

void dim_wrapped(const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(TEXT_DIM));
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

// Заголовок сторінки: велика назва й один рядок пояснення. Пояснення тут не
// прикраса — воно відповідає на «а це взагалі про що», доки людина не почала
// натискати навмання.
void page_title(const char* title, const char* what) {
    text_col(TEXT, title);
    ImGui::PushStyleColor(ImGuiCol_Text, col(TEXT_DIM));
    ImGui::TextWrapped("%s", what);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 10));
}

void field_label(const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(TEXT_FIELD));
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}

// Кольорова крапка стану. Малюємо самі, бо ані значка, ані картинки для цього
// не треба — коло є коло.
//
// Порядок незвичний: спершу лишаємо під крапку місце (dot_hold), потім
// малюємо напис — і аж тоді ставимо крапку навпроти нього (dot_at). Інакше
// ніяк: «середину рядка» наперед не порахувати, бо висоту рядка задає
// найвищий сусід — поле вводу, — а напис усередині неї стоїть по-своєму. Саме
// звідси бралися ті три пікселі, на які «вимкнено» й «читаємо» сиділи нижче
// за свої крапки.
ImVec2 dot_hold(float radius, float line_h) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(radius * 2.0f, line_h));
    return p;
}

// Рівень, на якому око шукає крапку поруч із написом, який щойно намалювали.
//
// Не середина коробки шрифта, а середина малих літер: у «вимкнено» немає ні
// виносних угору, ні хвостів униз, тож чорнило сидить нижче за ту середину —
// і крапка, поставлена по коробці, здається задертою. Беремо коробку самої
// літери «x»: це і є та смуга, яку бачить око, і міряється вона в поточному
// шрифті, тож лишається правильною й тоді, коли кегль зміниться.
float ink_center_y() {
    const ImVec2 a = ImGui::GetItemRectMin();
    if (ImFontBaked* baked = ImGui::GetFontBaked())
        if (const ImFontGlyph* g = baked->FindGlyphNoFallback((ImWchar)'x'))
            return a.y + (g->Y0 + g->Y1) * 0.5f;
    return (a.y + ImGui::GetItemRectMax().y) * 0.5f;
}

void dot_at(const ImVec2& p, float radius, ImU32 color) {
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + radius, ink_center_y()),
                                                radius, color);
}

ImU32 dot_color(const SourceView& s) {
    if (!s.configured) return DOT_OFF;
    if (s.connected) return DOT_LIVE;
    // «чекаю на ефір» — не помилка, це звичайний стан каналу, який не в ефірі.
    return s.note.find("…") != std::string::npos ? DOT_WAIT : DOT_ERR;
}

// Кнопка розділу «для досвідчених»: підпис ліворуч і справжній трикутник
// замість гліфа. «▸» (U+25B8) у Segoe UI немає — на його місці виходила
// порожня плитка, і це вже другий такий випадок.
bool disclosure(const char* label, bool open, float width) {
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.24f));
    char text[160];
    snprintf(text, sizeof text, "      %s", label);
    const bool r = ImGui::Button(text, ImVec2(width, 0));
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    const float cy = (a.y + b.y) * 0.5f;
    const float x = a.x + 14.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (open)
        dl->AddTriangleFilled(ImVec2(x - 4, cy - 2), ImVec2(x + 4, cy - 2),
                              ImVec2(x, cy + 4), ACCENT_DIM);
    else
        dl->AddTriangleFilled(ImVec2(x - 2, cy - 4), ImVec2(x + 4, cy),
                              ImVec2(x - 2, cy + 4), ACCENT_DIM);
    return r;
}

// Повзунок: доріжка, ПРОЙДЕНА ЧАСТИНА кольором, ручка, значення справа.
//
// Стандартний ImGui-повзунок малює лише ручку на порожній доріжці — і за нею не
// видно, багато це чи мало, доки не прочитаєш число. Залита ліва частина
// відповідає на це, ще не читаючи.
bool slider(const char* id, float* v, float lo, float hi, const char* fmt,
            float width) {
    const float h = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##t", ImVec2(width, h));
    const bool active = ImGui::IsItemActive();
    const bool hot = ImGui::IsItemHovered() || active;

    const float r = h * 0.30f;
    const float x0 = p.x + r, x1 = p.x + width - r;
    bool changed = false;
    if (active && x1 > x0) {
        float t = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float nv = lo + t * (hi - lo);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }
    const float t = hi > lo ? (*v - lo) / (hi - lo) : 0.0f;
    const float cy = p.y + h * 0.5f;
    const float kx = x0 + t * (x1 - x0);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), IM_COL32(255, 255, 255, hot ? 46 : 32), 4.0f);
    if (kx > x0) dl->AddLine(ImVec2(x0, cy), ImVec2(kx, cy), ACCENT, 4.0f);
    dl->AddCircleFilled(ImVec2(kx, cy), hot ? r : r - 1.0f, IM_COL32(244, 240, 255, 255));

    ImGui::SameLine(0, 10);
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *v);
    ImGui::AlignTextToFramePadding();
    text_col(IM_COL32(233, 213, 255, 255), buf);
    ImGui::PopID();
    return changed;
}

// Перемикач-тумблер із підписом і поясненням під ним. Стандартна «галочка»
// ImGui поруч зі скругленими картками виглядає як частина іншої програми.
bool toggle(const char* label, bool* on, const char* what = nullptr) {
    ImGui::PushID(label);
    // Ряд заввишки з кнопку, а сам тумблер — по його центру.
    //
    // Доки елемент був заввишки лише з тумблер, усе, що ставало поруч —
    // підпис, кнопки площадок — рахувало свою висоту від іншої величини, і ряд
    // розповзався: підпис виявлявся нижчим за тумблер, кнопки ще нижчими. Це і
    // є те «не на рівні», яке видно на кожній сторінці.
    const float row = ImGui::GetFrameHeight();
    const float h = row * 0.72f;
    const float w = h * 1.8f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p(p0.x, p0.y + (row - h) * 0.5f);

    const bool clicked = ImGui::InvisibleButton("##t", ImVec2(w, row));
    if (clicked) *on = !*on;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      *on ? ACCENT : IM_COL32(255, 255, 255, hovered ? 46 : 30),
                      h * 0.5f);
    const float r = h * 0.5f - 2.0f;
    dl->AddCircleFilled(ImVec2(*on ? p.x + w - r - 2.0f : p.x + r + 2.0f, p.y + h * 0.5f),
                        r, IM_COL32(255, 255, 255, 235));

    // Підпис, що починається з «##», — це лише ідентифікатор: сам перемикач
    // стоїть у ряду, де підписує його сусід ліворуч.
    if (label[0] != '#' || label[1] != '#') {
        ImGui::SameLine(0, 10);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
    }
    // Пояснення — рядком нижче, але з відступом ПІД ПІДПИС, а не під тумблер:
    // так видно, що воно належить саме цьому перемикачу. Поруч у тому ж рядку
    // воно розтягувало ряд на три рядки й ламало вирівнювання всієї сторінки.
    if (what) {
        ImGui::Indent(w + 10.0f);
        dim_wrapped(what);
        ImGui::Unindent(w + 10.0f);
    }
    ImGui::PopID();
    return clicked;
}

// Рядок рейки. Активний позначений смужкою акценту ліворуч — це видно краєм
// ока, на відміну від самої лише зміни кольору тексту.
bool rail_item(const char* label, bool active, bool badge) {
    ImGui::PushID(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 36.0f;

    const bool clicked = ImGui::InvisibleButton("##i", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || hovered)
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          IM_COL32(255, 255, 255, active ? 20 : 10));
    if (active)
        dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + 3, p.y + h), ACCENT);

    text_at(dl, p.x + 18.0f, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f,
            active ? TEXT : IM_COL32(170, 170, 180, 255), label);
    if (badge)
        dl->AddCircleFilled(ImVec2(p.x + w - 16, p.y + h * 0.5f), 3.5f, ACCENT_DIM);
    ImGui::PopID();
    return clicked;
}

void copy_to(char* dst, size_t cap, const std::string& src) {
    const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    memcpy(dst, src.data(), n);
    dst[n] = 0;
}

// Опис випуску: він пишеться людиною в кілька рядків, зі списками й
// підзаголовками. Показати його одним абзацом означає злити все докупи — і
// написане «по пунктах» читається як суцільне полотно.
//
// Правил рівно три, за тим, як ці описи й пишуть:
//   • рядок на «•» — пункт списку, перенос іде під текст, а не під маркер;
//   • рядок КАПСОМ — підзаголовок розділу;
//   • порожній рядок — кінець абзацу.
void draw_notes(const std::string& text) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        const bool last = end == std::string::npos;
        if (last) end = text.size();
        std::string line = trimmed(text.substr(pos, end - pos));
        pos = end + 1;

        if (line.empty()) {
            ImGui::Dummy(ImVec2(0, 6));
            if (last) break;
            continue;
        }

        // Підзаголовок: літери є, і серед них немає рядкових. Кирилицю
        // розбираємо самі — std::toupper на UTF-8 побайтово не працює.
        //
        // Межі саме такі, і українські літери тут не дрібниця: «і» (U+0456),
        // «ї» (U+0457) і «є» (U+0454) лежать у D1 90..9F, а не в D1 80..8F, як
        // решта рядкових. Взяти лише другий діапазон означало б вважати рядок
        // «і це важливо» підзаголовком.
        bool has_lower = false, has_letter = false;
        for (size_t i = 0; i < line.size() && !has_lower; ++i) {
            const unsigned char c = (unsigned char)line[i];
            const unsigned char n = i + 1 < line.size() ? (unsigned char)line[i + 1] : 0;
            if (c >= 'a' && c <= 'z') { has_lower = has_letter = true; }
            else if (c >= 'A' && c <= 'Z') has_letter = true;
            else if (c == 0xD0) {            // А-Я: 90..AF, а-п: B0..BF
                has_letter = true;
                has_lower = n >= 0xB0;
                ++i;
            } else if (c == 0xD1) {          // р-я: 80..8F, ё/є/і/ї…: 90..9F
                has_letter = true;
                has_lower = n <= 0x9F;
                ++i;
            } else if (c == 0xD2 || c == 0xD3) {   // ґ/Ґ і сусіди: непарний — мала
                has_letter = true;
                has_lower = (n & 1) != 0;
                ++i;
            }
        }
        if (has_letter && !has_lower && line.size() > 3) {
            ImGui::Dummy(ImVec2(0, 6));
            text_col(ACCENT_DIM, line.c_str());
            continue;
        }

        if (line.compare(0, 3, "•") == 0) {          // «•», три байти в UTF-8
            // Висячий відступ: другий рядок пункту стає під текст, а не під
            // маркер, і список лишається списком.
            ImGui::Indent(14.0f);
            dim_wrapped(line.c_str());
            ImGui::Unindent(14.0f);
            continue;
        }
        dim_wrapped(line.c_str());
        if (last) break;
    }
}

// --- сторінки ---------------------------------------------------------------

// Один рядок каналу: підпис, поле, крапка стану. Однакова сітка на всі
// чотири — око знаходить потрібне поле, не читаючи підписів.
bool channel_row(const char* label, const char* hint, char* buf, size_t cap,
                 const SourceView* src, float field_w) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    field_label(label);
    ImGui::SameLine(LABEL_W);

    ImGui::SetNextItemWidth(field_w);
    const bool done = ImGui::InputTextWithHint("##f", hint, buf, cap,
                                               ImGuiInputTextFlags_EnterReturnsTrue) ||
                      ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine(0, 10);
    if (src) {
        const ImVec2 dp = dot_hold(4.0f, ImGui::GetFrameHeight());
        ImGui::SameLine(0, 7);
        ImGui::AlignTextToFramePadding();
        if (!src->configured) text_col(TEXT_DIM, "вимкнено");
        else if (src->viewers_known) text_col(TEXT, src->viewers.c_str());
        else if (src->connected) text_col(TEXT_DIM, "читаємо");
        else text_col(TEXT_DIM, "…");
        dot_at(dp, 4.0f, dot_color(*src));
        if (ImGui::IsItemHovered()) {
            if (src->viewers_known)
                ImGui::SetTooltip("глядачів зараз%s%s", src->note.empty() ? "" : " · ",
                                  src->note.c_str());
            else if (!src->note.empty())
                ImGui::SetTooltip("%s", src->note.c_str());
        }
    }
    ImGui::PopID();
    return done;
}

void page_channels(SettingsState* st, Config* cfg, const std::vector<SourceView>& src,
                   SettingsEvents* ev) {
    page_title("Канали", "Звідки читати чат. Можна кілька одразу — усе піде в одну стрічку.");

    const float field_w = ImGui::GetContentRegionAvail().x - LABEL_W - 110.0f;
    const SourceView* s0 = src.size() > 0 ? &src[0] : nullptr;
    const SourceView* s1 = src.size() > 1 ? &src[1] : nullptr;
    const SourceView* s2 = src.size() > 2 ? &src[2] : nullptr;
    const SourceView* s3 = src.size() > 3 ? &src[3] : nullptr;

    if (channel_row("Twitch", "нік або twitch.tv/канал", st->twitch, sizeof st->twitch,
                    s0, field_w)) {
        cfg->twitch = st->twitch;
        ev->changed = ev->sources_changed = true;
    }
    if (channel_row("Kick", "нік або kick.com/канал", st->kick, sizeof st->kick,
                    s1, field_w)) {
        cfg->kick = st->kick;
        ev->changed = ev->sources_changed = true;
    }
    if (channel_row("YouTube", "@нік, посилання або UC…", st->youtube, sizeof st->youtube,
                    s2, field_w)) {
        cfg->youtube = st->youtube;
        ev->changed = ev->sources_changed = true;
    }
    if (channel_row("Свій сайт", "посилання на сторінку вашого чату", st->site,
                    sizeof st->site, s3, field_w)) {
        cfg->site_url = st->site;
        ev->changed = ev->sources_changed = true;
    }
    ImGui::Indent(LABEL_W);
    dim_wrapped("Читаємо не сторінку, а той самий websocket, яким користується вона сама.");
    ImGui::Unindent(LABEL_W);

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::AlignTextToFramePadding();
    field_label("Глядачі");
    ImGui::SameLine(LABEL_W);
    {
        // Спершу вимикач, і лише потім — кого рахувати. Вимкнений лічильник
        // лишає вибір площадок як був: повернути його з тими самими площадками
        // — звичайна річ, і складати їх щоразу наново було б безглуздо.
        if (toggle("##viewers", &cfg->viewers_show)) ev->changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(cfg->viewers_show ? "Не показувати глядачів"
                                                : "Показувати глядачів");

        // Поки вимкнено — решта ряду сіра й не натискається: видно, що вона є,
        // але зараз ні на що не впливає.
        ImGui::BeginDisabled(!cfg->viewers_show);
        struct Item { const char* name; bool* on; };
        const Item items[] = {{"Twitch", &cfg->viewers_twitch},
                              {"Kick", &cfg->viewers_kick},
                              {"YouTube", &cfg->viewers_youtube}};
        for (int i = 0; i < 3; ++i) {
            ImGui::SameLine(0, i ? 5 : 10);
            const bool on = *items[i].on;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? col(ACCENT) : ImVec4(1, 1, 1, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  on ? col(ACCENT) : ImVec4(1, 1, 1, 0.16f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(ACCENT));
            if (ImGui::Button(items[i].name, ImVec2(72, 0))) {
                *items[i].on = !on;
                ev->changed = true;
            }
            ImGui::PopStyleColor(3);
        }
        ImGui::EndDisabled();
    }
    ImGui::Indent(LABEL_W);
    dim_wrapped("Одне число з усіх позначених площадок. Видно у смужці вікна чату, "
                "а коли смужка вимкнена — плашкою в кутку.");
    ImGui::Unindent(LABEL_W);

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::AlignTextToFramePadding();
    field_label("Затримка");
    ImGui::SameLine(LABEL_W);
    float delay = (float)cfg->chat_delay;
    if (slider("##delay", &delay, 0.0f, 60.0f, "%.0f с", field_w)) {
        cfg->chat_delay = (int)(delay + 0.5f);
        ev->changed = ev->sources_changed = true;
    }
    ImGui::Indent(LABEL_W);
    dim_wrapped("Видає повідомлення по одному, коли пишуть швидше, ніж читаєш.");
    ImGui::Unindent(LABEL_W);
}

void page_look(Config* cfg, SettingsEvents* ev) {
    page_title("Вигляд", "Як виглядає вікно чату. Змінюється одразу — дивіться на нього.");

    const float slider_w = ImGui::GetContentRegionAvail().x - LABEL_W - 70.0f;


    ImGui::AlignTextToFramePadding();
    field_label("Прозорість");
    ImGui::SameLine(LABEL_W);
    float op = cfg->look.opacity * 100.0f;
    if (slider("##op", &op, 25.0f, 100.0f, "%.0f%%", slider_w)) {
        cfg->look.opacity = op / 100.0f;
        ev->changed = ev->look_changed = true;
    }

    ImGui::AlignTextToFramePadding();
    field_label("Тло");
    ImGui::SameLine(LABEL_W);
    float bg = cfg->look.bg_alpha * 100.0f;
    if (slider("##bg", &bg, 0.0f, 100.0f, "%.0f%%", slider_w)) {
        cfg->look.bg_alpha = bg / 100.0f;
        ev->changed = ev->look_changed = true;
    }

    ImGui::AlignTextToFramePadding();
    field_label("Текст");
    ImGui::SameLine(LABEL_W);
    if (ghost("A−", 40.0f)) {
        cfg->look.zoom = zoom_clamp(cfg->look.zoom - ZOOM_STEP);
        ev->changed = ev->look_changed = true;
    }
    ImGui::SameLine(0, 8);
    {
        char buf[16];
        snprintf(buf, sizeof buf, "%d%%", (int)(cfg->look.zoom * 100.0f + 0.5f));
        ImGui::AlignTextToFramePadding();
        text_col(IM_COL32(233, 213, 255, 255), buf);
    }
    ImGui::SameLine(0, 8);
    if (ghost("A+", 40.0f)) {
        cfg->look.zoom = zoom_clamp(cfg->look.zoom + ZOOM_STEP);
        ev->changed = ev->look_changed = true;
    }

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::AlignTextToFramePadding();
    field_label("Анімація");
    ImGui::SameLine(LABEL_W);
    {
        static const char* kIds[] = {"play", "freeze", "hide"};
        static const char* kNames[] = {"Грає", "Нерухомі", "Приховати"};
        // Стосується не лише емоутів: гіфки з чату Twitch ідуть тим самим
        // шляхом, і вимикається їх рух тут же.
        static const char* kHints[] = {
            "Як задумав автор емоута. Гіфки з чату Twitch теж рухаються.",
            "Лишається перший кадр — і в емоутів, і в гіфок. Менше памʼяті й "
            "жодного перемальовування.",
            "Анімованих емоутів і гіфок не видно зовсім — у рядку лишається їх код.",
        };
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, 6);
            const bool on = cfg->motion == kIds[i];
            ImGui::PushStyleColor(ImGuiCol_Button, on ? col(ACCENT) : ImVec4(1, 1, 1, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  on ? col(ACCENT) : ImVec4(1, 1, 1, 0.16f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(ACCENT));
            if (ImGui::Button(kNames[i], ImVec2(96, 0))) {
                cfg->motion = kIds[i];
                ev->changed = ev->motion_changed = true;
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kHints[i]);
        }
    }

    ImGui::Dummy(ImVec2(0, 10));
    {
        // Тло веде повзунок «Тло» вище — воно лишається завжди, зокрема поверх
        // гри. Тут лише рамка. Доки це був один перемикач «Без рамки», разом із
        // рамкою зникало й тло, і повзунок переставав робити будь-що.
        bool border = !cfg->look.frameless;
        if (toggle("Рамка навколо вікна", &border,
                   "Фіолетова смужка по краю (зелена, коли вікно замкнене). Тло під "
                   "чатом від неї не залежить — його веде «Тло» вище.")) {
            cfg->look.frameless = !border;
            ev->changed = ev->look_changed = true;
        }
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (toggle("Смужка згори", &cfg->header,
               "Заголовок вікна: назва, замок, повзунки, налаштування. Вимкнена — "
               "лишаються самі повідомлення, а керування з'являється під курсором.")) {
        ev->changed = ev->look_changed = true;
    }

    // Замок — це «не заважай мишею», а не «зникни». Далі думки розходяться:
    // одному потрібне зовсім чисте вікно поверх гри, другому — число глядачів
    // перед очима весь ефір. Тож не вгадуємо, а питаємо.
    ImGui::Dummy(ImVec2(0, 14));
    field_label("Коли вікно замкнене");
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Indent(10.0f);
    // Свій простір імен: нижче є «Смужка згори», і такий самий підпис уже є
    // вище, на рівні вікна. Для ImGui підпис — це ще й ідентифікатор, тож два
    // однакові означали б два перемикачі з одним станом.
    ImGui::PushID("lock");
    if (toggle("Тло під чатом", &cfg->look.lock_bg,
               "Підкладка й рамка. Вимкнено — поверх гри лишаються самі "
               "повідомлення, без жодного прямокутника.")) {
        ev->changed = ev->look_changed = true;
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (toggle("Глядачі", &cfg->look.lock_viewers,
               "Плашка з числом глядачів у кутку. Показується, лише поки "
               "лічильник увімкнено в розділі «Канали».")) {
        ev->changed = ev->look_changed = true;
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (toggle("Смужка згори", &cfg->look.lock_header,
               "Заголовок із назвою, замком і повзунками. Видно його буде, а "
               "натиснути — ні: під замком миша проходить крізь вікно.")) {
        ev->changed = ev->look_changed = true;
    }
    ImGui::PopID();
    ImGui::Unindent(10.0f);

    ImGui::Dummy(ImVec2(0, 14));
    if (toggle("Поверх усіх вікон", &cfg->keep_top,
               "У рідкісних старих іграх це дає мерехтіння — тоді вимкніть."))
        ev->changed = true;

    ImGui::Dummy(ImVec2(0, 12));
    if (ghost("Свій CSS для чату…", 200.0f)) ev->css_editor = true;
    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    dim_wrapped("Окреме вікно: код, довідник класів і перевірка.");
}

void page_game(SettingsState* st, Config* cfg, const GameView& game, SettingsEvents* ev) {
    page_title("Чат поверх гри",
               "Це вікно й так видно поверх майже будь-якої гри — і OBS його не знімає. "
               "Нижче те, що потрібно, коли гра забирає екран собі.");

    const float full = ImGui::GetContentRegionAvail().x;

    ImGui::AlignTextToFramePadding();
    field_label("Гра");
    ImGui::SameLine(LABEL_W);
    ImGui::SetNextItemWidth(full - LABEL_W - 96.0f);
    const char* current = game.windows.empty()
                              ? "Немає відкритих ігор — запустіть гру й оновіть"
                              : game.windows[(size_t)game.picked].c_str();
    if (ImGui::BeginCombo("##game", current)) {
        for (size_t i = 0; i < game.windows.size(); ++i) {
            const bool on = (int)i == game.picked;
            if (ImGui::Selectable(game.windows[i].c_str(), on)) ev->pick_game = (int)i;
            if (on) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 8);
    if (ghost("Оновити", 80.0f)) ev->refresh_games = true;

    ImGui::Dummy(ImVec2(0, 12));
    if (!game.windows.empty()) {
        // Повноекранна оптимізація — не ворог, а єдина причина, з якої чат
        // взагалі видно поверх повноекранної гри.
        //
        // Тут довго стояла кнопка «Прибрати повноекранну оптимізацію», і вона
        // робила рівно протилежне обіцяному. З увімкненою оптимізацією Windows
        // підміняє грі виключний повноекран безрамковим вікном, і щойно згори
        // з'являється чуже вікно — композитор перестає віддавати кадр напряму
        // й малює нас поверх. Без неї гра забирає екран собі, композитора на
        // цьому моніторі немає взагалі — і показувати наше вікно нікому. Ані
        // «поверх усіх вікон», ані DirectComposition тут не значать нічого.
        //
        // Тому вимкнути її звідси більше не можна: кнопка є лише тоді, коли
        // прапорець уже стоїть, і лише щоб його зняти.
        if (game.fso_off) {
            if (ghost("Увімкнути повноекранну оптимізацію", 320.0f)) ev->fix_fso = true;
            ImGui::SameLine(0, 12);
            ImGui::AlignTextToFramePadding();
            dim_wrapped("Для цієї гри вона вимкнена — у повноекранному чату не буде видно.");
        } else {
            ImGui::AlignTextToFramePadding();
            dim_wrapped("Повноекранна оптимізація для цієї гри увімкнена — саме завдяки їй "
                        "чат видно поверх гри. Нічого міняти не треба.");
        }

        ImGui::Dummy(ImVec2(0, 6));
        if (ghost("Зробити безрамковою", 180.0f)) ev->make_borderless = true;
        if (game.restorable) {
            ImGui::SameLine(0, 8);
            if (ghost("Повернути", 92.0f)) ev->restore_window = true;
        }
        ImGui::SameLine(0, 12);
        ImGui::AlignTextToFramePadding();
        dim_wrapped("Знімає рамку й розтягує на монітор.");
    }

    // Інжект — за окремим кроком. Найпотужніше й найризикованіше, і натрапити
    // на нього випадково не можна.
    if (game.injector) {
        ImGui::Dummy(ImVec2(0, 16));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));
        if (disclosure("Для досвідчених: чат усередині гри", st->show_injector, full))
            st->show_injector = !st->show_injector;

        if (st->show_injector) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(252, 211, 77, 255)));
            ImGui::TextWrapped("Вкладає бібліотеку в процес гри. В ОНЛАЙН-іграх з "
                               "античитом так робити НЕ можна — це загроза бану.");
            ImGui::PopStyleColor();
            dim_wrapped("Потрібне рідко: спосіб вище працює майже скрізь. Зате малює чат "
                        "усередині кадру навіть у виключному повноекранному.");
            ImGui::Dummy(ImVec2(0, 8));
            if (!game.windows.empty() &&
                ghost(game.injected ? "Показати ще раз" : "Показати чат у грі", 200.0f))
                ev->inject = true;
            if (game.injected) {
                ImGui::SameLine(0, 8);
                if (ghost("Прибрати", 100.0f)) ev->stop_inject = true;

                ImGui::Dummy(ImVec2(0, 10));
                ImGui::AlignTextToFramePadding();
                field_label("Прозорість");
                ImGui::SameLine(LABEL_W);
                float op = (float)cfg->game_opacity;
                if (slider("##gop", &op, 30.0f, 255.0f, "%.0f", full - LABEL_W - 70.0f)) {
                    cfg->game_opacity = (int)(op + 0.5f);
                    ev->changed = true;
                }
                ImGui::Dummy(ImVec2(0, 6));
                if (toggle("Ховати чат від OBS", &cfg->game_hide_obs)) ev->changed = true;
            }
        }
    }

    if (!game.status.empty()) {
        ImGui::Dummy(ImVec2(0, 12));
        dim_wrapped(game.status.c_str());
    }
}

void page_update(Config* cfg, const UpdateView& upd, SettingsEvents* ev) {
    page_title("Оновлення", "Звідки брати нові версії і чи перевіряти самим.");

    ImGui::AlignTextToFramePadding();
    field_label("Канал");
    ImGui::SameLine(LABEL_W);
    static const char* kIds[] = {"stable", "beta", "dev"};
    static const char* kNames[] = {"Стабільна", "Бета", "Тестова"};
    static const char* kHints[] = {
        "Перевірені випуски. Рекомендовано.",
        "Свіжі можливості до того, як вони потраплять у стабільну.",
        "Збірки одразу після змін. Можуть ламатися.",
    };
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, 6);
        const bool on = cfg->channel == kIds[i];
        ImGui::PushStyleColor(ImGuiCol_Button, on ? col(ACCENT) : ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              on ? col(ACCENT) : ImVec4(1, 1, 1, 0.16f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(ACCENT));
        if (ImGui::Button(kNames[i], ImVec2(104, 0))) {
            cfg->channel = kIds[i];
            ev->changed = true;
            ev->check_update = true;   // канал інший — і випуск може бути інший
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kHints[i]);
    }

    ImGui::Dummy(ImVec2(0, 12));
    if (toggle("Перевіряти автоматично", &cfg->auto_update)) ev->changed = true;

    ImGui::Dummy(ImVec2(0, 14));
    if (upd.percent >= 0) {
        ImGui::ProgressBar(upd.percent / 100.0f,
                           ImVec2(ImGui::GetContentRegionAvail().x, 6.0f), "");
        ImGui::Dummy(ImVec2(0, 6));
    }

    // Стан — одним рядком. Коли є що ставити, рядок мовчить: назва випуску
    // нижче каже те саме, тільки конкретніше, а два повідомлення про одне —
    // це вже шум.
    if (upd.title.empty()) dim_wrapped(upd.status.c_str());

    if (!upd.title.empty() || !upd.notes.empty()) {
        if (upd.title.empty()) ImGui::Dummy(ImVec2(0, 8));
        ImGui::BeginChild("##upd", ImVec2(0, ImGui::GetContentRegionAvail().y - 52.0f),
                          true);
        if (!upd.title.empty()) {
            text_col(TEXT, upd.title.c_str());
            if (!upd.size.empty()) {
                ImGui::SameLine(0, 10);
                text_col(TEXT_DIM, upd.size.c_str());
            }
            ImGui::Dummy(ImVec2(0, 4));
        }
        if (upd.mandatory)
            text_col(IM_COL32(252, 211, 77, 255),
                     "Це виправлення важливе — краще поставити.");
        if (!upd.warning.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(252, 211, 77, 255)));
            ImGui::TextWrapped("%s", upd.warning.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));
        }
        if (!upd.notes.empty()) {
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 4));
            draw_notes(upd.notes);
        }
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0, 4));
    }

    if (upd.can_download) {
        // Розмір у самій кнопці: качати три мегабайти й качати двісті — різні
        // рішення, і людина має ухвалювати його до натискання, а не після.
        char label[64];
        snprintf(label, sizeof label, "Завантажити%s%s", upd.size.empty() ? "" : " ",
                 upd.size.c_str());
        if (ghost(label, 220.0f)) ev->start_download = true;
        ImGui::SameLine(0, 8);
    }
    if (upd.can_install) {
        if (ghost("Встановити й перезапустити", 240.0f)) ev->do_install = true;
        ImGui::SameLine(0, 8);
    }
    if (upd.can_check && ghost("Перевірити", 130.0f)) ev->check_update = true;
}

void page_about(const std::string& facts) {
    page_title("Про програму", HOMINKA_NAME " " HOMINKA_VERSION);
    dim_wrapped("Чат читається й малюється самою програмою: ані браузера, ані Python. "
                "Тому вона важить кілька мегабайтів і не займає відеокарту, поки в "
                "чаті тихо.");
    ImGui::Dummy(ImVec2(0, 14));
    field_label("Зараз");
    dim_wrapped(facts.c_str());
    ImGui::Dummy(ImVec2(0, 14));
    field_label("Гарячі клавіші");
    dim_wrapped("Ctrl+Alt+Space — замок: миша перестає помічати вікно чату.");
}

}  // namespace

void SettingsState::sync(const Config& cfg) {
    copy_to(youtube, sizeof youtube, cfg.youtube);
    copy_to(twitch, sizeof twitch, cfg.twitch);
    copy_to(kick, sizeof kick, cfg.kick);
    copy_to(site, sizeof site, cfg.site_url);
    synced = true;
}

void settings_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.WindowBorderSize = 0.0f;
    s.WindowPadding = ImVec2(0, 0);
    s.FrameRounding = 7.0f;
    s.GrabRounding = 7.0f;
    s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(8, 9);
    s.ScrollbarRounding = 4.0f;
    s.ScrollbarSize = 9.0f;
    s.GrabMinSize = 12.0f;
    s.PopupRounding = 8.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = col(IM_COL32(14, 15, 18, 255));
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = col(IM_COL32(24, 24, 30, 250));
    c[ImGuiCol_Text] = col(TEXT);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.09f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_SliderGrab] = col(ACCENT);
    c[ImGuiCol_SliderGrabActive] = col(IM_COL32(192, 132, 252, 255));
    c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.16f);
    c[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.24f);
    c[ImGuiCol_Header] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.16f);
    c[ImGuiCol_HeaderActive] = col(ACCENT);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.18f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.30f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    // Стандартна обводка темної теми — світло-сіра, і на чорному тлі вона
    // читається як біла рамка навколо кожної картки. Своя, ледь помітна.
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PlotHistogram] = col(ACCENT);
}

SettingsEvents draw_settings(SettingsState* st, Config* cfg,
                             const std::vector<SourceView>& sources,
                             const UpdateView& upd, const GameView& game,
                             const std::string& facts, int w, int h) {
    SettingsEvents ev;
    if (!st->synced) st->sync(*cfg);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::Begin("##settings", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float body_h = (float)h - TITLE_H - STATUS_H;

    // --- заголовок -----------------------------------------------------------
    dl->AddRectFilled(org, ImVec2(org.x + w, org.y + TITLE_H), TITLE_BG);
    dl->AddLine(ImVec2(org.x, org.y + TITLE_H), ImVec2(org.x + w, org.y + TITLE_H),
                IM_COL32(168, 85, 247, 90));

    {
        // Значок перед назвою — той самий, що у смужці вікна чату й на ярлику.
        // Заголовок без нього читався як чужа панель, приклеєна збоку.
        const float ico = 20.0f;
        app_logo(dl, org.x + PAD, org.y + (TITLE_H - ico) * 0.5f, ico);
    }
    ImGui::SetCursorPos(ImVec2(PAD + 28.0f, (TITLE_H - ImGui::GetTextLineHeight()) * 0.5f));
    text_col(TEXT, "Hominka");
    ImGui::SameLine(0, 10);
    text_col(TEXT_DIM, HOMINKA_VERSION);

    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##title", ImVec2((float)w - 48.0f, TITLE_H));
    ev.title_active = ImGui::IsItemActive();

    if (close_button((float)w - 38.0f, (TITLE_H - 30.0f) * 0.5f, 30.0f)) ev.close = true;

    // --- рейка розділів ------------------------------------------------------
    dl->AddRectFilled(ImVec2(org.x, org.y + TITLE_H),
                      ImVec2(org.x + RAIL_W, org.y + TITLE_H + body_h), RAIL_BG);
    dl->AddLine(ImVec2(org.x + RAIL_W, org.y + TITLE_H),
                ImVec2(org.x + RAIL_W, org.y + TITLE_H + body_h), LINE);

    ImGui::SetCursorPos(ImVec2(0, TITLE_H + 8));
    ImGui::BeginChild("##rail", ImVec2(RAIL_W, body_h - 8.0f), false);
    {
        // Крапка на розділі — це «сюди варто заглянути»: джерело з помилкою або
        // готове оновлення. Більше ніде вона не з'являється, тож не шумить.
        bool bad_source = false;
        for (const SourceView& s : sources)
            if (s.configured && !s.connected &&
                s.note.find("…") == std::string::npos)
                bad_source = true;

        if (rail_item("Канали", st->page == 0, bad_source)) st->page = 0;
        if (rail_item("Вигляд", st->page == 1, false)) st->page = 1;
        if (game.supported && rail_item("Поверх гри", st->page == 2, false)) st->page = 2;
        if (upd.supported && rail_item("Оновлення", st->page == 3, upd.available))
            st->page = 3;
        if (rail_item("Про програму", st->page == 4, false)) st->page = 4;
    }
    ImGui::EndChild();

    // --- сторінка ------------------------------------------------------------
    ImGui::SetCursorPos(ImVec2(RAIL_W + PAD, TITLE_H + PAD));
    // «Канали» вміщаються цілком, і смужка прокрутки там — лише зайва деталь,
    // що натякає, ніби нижче щось є. Решті розділів вона потрібна.
    ImGui::BeginChild("##page", ImVec2((float)w - RAIL_W - PAD * 2, body_h - PAD * 2),
                      false,
                      st->page == 0 ? ImGuiWindowFlags_NoScrollbar
                                    : ImGuiWindowFlags_None);
    switch (st->page) {
    case 0: page_channels(st, cfg, sources, &ev); break;
    case 1: page_look(cfg, &ev); break;
    case 2: page_game(st, cfg, game, &ev); break;
    case 3: page_update(cfg, upd, &ev); break;
    default: page_about(facts); break;
    }
    ImGui::EndChild();

    // --- смужка стану --------------------------------------------------------
    //
    // Головне питання до такої програми — «вона зараз щось читає?». Відповідь
    // має бути видно з будь-якого розділу, не гортаючи назад.
    const float sy = org.y + (float)h - STATUS_H;
    dl->AddRectFilled(ImVec2(org.x, sy), ImVec2(org.x + w, org.y + h), TITLE_BG);
    dl->AddLine(ImVec2(org.x, sy), ImVec2(org.x + w, sy), LINE);

    ImGui::SetCursorPos(ImVec2(PAD, (float)h - STATUS_H + 7.0f));
    bool first = true;
    for (const SourceView& s : sources) {
        if (!s.configured) continue;
        if (!first) ImGui::SameLine(0, 16);
        first = false;
        const ImVec2 dp = dot_hold(3.5f, ImGui::GetTextLineHeight());
        ImGui::SameLine(0, 7);
        text_col(s.connected ? TEXT : TEXT_DIM, s.name.c_str());
        dot_at(dp, 3.5f, dot_color(s));
        if (ImGui::IsItemHovered() && !s.note.empty())
            ImGui::SetTooltip("%s", s.note.c_str());
    }
    if (first) text_col(TEXT_DIM, "Жодного каналу не вказано — почніть з розділу «Канали».");

    ImGui::End();
    return ev;
}

}  // namespace hominka
