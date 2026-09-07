#include "ui/settings_ui.h"

#include <cstdio>
#include <cstring>

#include "core/version.h"
#include "imgui/imgui.h"

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
const float RAIL_W = 168.0f;
const float STATUS_H = 30.0f;
const float PAD = 18.0f;
const float LABEL_W = 92.0f;      // стовпчик підписів у розділі «Канали»

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

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
void dot(ImU32 color, float radius = 4.0f) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + radius, p.y + h * 0.5f), radius, color);
    ImGui::Dummy(ImVec2(radius * 2.0f, h));
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

bool ghost(const char* label, float width = 0.0f) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.24f));
    const bool r = ImGui::Button(label, ImVec2(width, 0));
    ImGui::PopStyleColor(3);
    return r;
}

// Повзунок із підписом справа: значення поруч, а не всередині доріжки —
// інакше його не видно на світлій частині.
bool slider(const char* id, float* v, float lo, float hi, const char* fmt,
            float width) {
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::SliderFloat(id, v, lo, hi, "");
    ImGui::SameLine(0, 10);
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *v);
    ImGui::AlignTextToFramePadding();
    text_col(IM_COL32(233, 213, 255, 255), buf);
    return changed;
}

// Перемикач-тумблер із підписом і поясненням під ним. Стандартна «галочка»
// ImGui поруч зі скругленими картками виглядає як частина іншої програми.
bool toggle(const char* label, bool* on, const char* what = nullptr) {
    ImGui::PushID(label);
    const float h = ImGui::GetFrameHeight() * 0.82f;
    const float w = h * 1.8f;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    const bool clicked = ImGui::InvisibleButton("##t", ImVec2(w, h));
    if (clicked) *on = !*on;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      *on ? ACCENT : IM_COL32(255, 255, 255, hovered ? 46 : 30),
                      h * 0.5f);
    const float r = h * 0.5f - 2.0f;
    dl->AddCircleFilled(ImVec2(*on ? p.x + w - r - 2.0f : p.x + r + 2.0f, p.y + h * 0.5f),
                        r, IM_COL32(255, 255, 255, 235));

    ImGui::SameLine(0, 10);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
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
        dl->AddRectFilled(ImVec2(p.x, p.y + 6), ImVec2(p.x + 3, p.y + h - 6), ACCENT, 1.5f);

    dl->AddText(ImVec2(p.x + 18, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
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
        ImGui::AlignTextToFramePadding();
        dot(dot_color(*src));
        ImGui::SameLine(0, 7);
        ImGui::AlignTextToFramePadding();
        if (!src->configured) text_col(TEXT_DIM, "вимкнено");
        else if (src->connected) text_col(TEXT_DIM, "читаємо");
        else text_col(TEXT_DIM, "…");
        if (ImGui::IsItemHovered() && !src->note.empty())
            ImGui::SetTooltip("%s", src->note.c_str());
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
    dim_wrapped("Тримає повідомлення й видає їх по одному — коли пишуть швидше, ніж "
                "читаєш, стрічка перестає бути кашею. 0 — без затримки.");
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
        cfg->look.zoom = cfg->look.zoom - 0.1f < 0.5f ? 0.5f : cfg->look.zoom - 0.1f;
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
        cfg->look.zoom = cfg->look.zoom + 0.1f > 3.0f ? 3.0f : cfg->look.zoom + 0.1f;
        ev->changed = ev->look_changed = true;
    }

    ImGui::Dummy(ImVec2(0, 10));
    if (toggle("Без рамки", &cfg->look.frameless,
               "Лише повідомлення: ні підкладки, ні рамки. Керування повертається, "
               "щойно знімеш замок."))
        ev->changed = ev->look_changed = true;

    ImGui::Dummy(ImVec2(0, 6));
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
        if (ghost(game.fso_off ? "Повернути повноекранну оптимізацію"
                               : "Прибрати повноекранну оптимізацію", 280.0f))
            ev->toggle_fso = true;
        ImGui::SameLine(0, 12);
        ImGui::AlignTextToFramePadding();
        dim_wrapped("Коли чат видно в меню, а в бою зникає.");

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

    ImGui::Dummy(ImVec2(0, 16));
    if (upd.mandatory)
        text_col(IM_COL32(252, 211, 77, 255), "Це виправлення важливе — краще поставити.");
    if (upd.percent >= 0) {
        ImGui::ProgressBar(upd.percent / 100.0f,
                           ImVec2(ImGui::GetContentRegionAvail().x, 6.0f), "");
        ImGui::Dummy(ImVec2(0, 6));
    }

    // Опис випуску буває довгим — йому окреме місце з прокруткою, щоб він не
    // виштовхував кнопки за нижній край.
    // Висота — по вмісту, зі стелею: короткому рядку не потрібна половина
    // сторінки, а довгий опис випуску не має виштовхувати кнопки за край.
    {
        const float want = ImGui::CalcTextSize(upd.status.c_str(), nullptr, false,
                                               ImGui::GetContentRegionAvail().x).y + 8.0f;
        const float cap = ImGui::GetContentRegionAvail().y - 46.0f;
        ImGui::BeginChild("##upd", ImVec2(0, want < cap ? want : cap), false);
        dim_wrapped(upd.status.c_str());
        ImGui::EndChild();
    }
    ImGui::Dummy(ImVec2(0, 4));

    if (upd.can_download) {
        if (ghost("Завантажити оновлення", 220.0f)) ev->start_download = true;
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

    ImGui::SetCursorPos(ImVec2(PAD, 12));
    text_col(TEXT, "Hominka");
    ImGui::SameLine(0, 10);
    text_col(TEXT_DIM, HOMINKA_VERSION);

    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##title", ImVec2((float)w - 44.0f, TITLE_H));
    ev.title_active = ImGui::IsItemActive();

    ImGui::SetCursorPos(ImVec2((float)w - 36.0f, 9));
    if (ghost("×", 24.0f)) ev.close = true;

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
    ImGui::BeginChild("##page", ImVec2((float)w - RAIL_W - PAD * 2, body_h - PAD * 2),
                      false);
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
        dot(dot_color(s), 3.5f);
        ImGui::SameLine(0, 7);
        text_col(s.connected ? TEXT : TEXT_DIM, s.name.c_str());
        if (ImGui::IsItemHovered() && !s.note.empty())
            ImGui::SetTooltip("%s", s.note.c_str());
    }
    if (first) text_col(TEXT_DIM, "Жодного каналу не вказано — почніть з розділу «Канали».");

    ImGui::End();
    return ev;
}

}  // namespace hominka
