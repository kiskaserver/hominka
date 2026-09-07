#include "settings_ui.h"

#include <cstdio>
#include <cstring>

#include "imgui/imgui.h"
#include "version.h"

namespace hominka {

namespace {

// Ті самі кольори, що були у Qt-панелі (hominka/styles.py). Люди впізнають
// програму по них, і міняти їх лише тому, що змінився рушій, — привід
// пояснювати старим користувачам, що нічого не зламалося.
const ImU32 ACCENT     = IM_COL32(168, 85, 247, 255);   // #a855f7
const ImU32 ACCENT_DIM = IM_COL32(196, 181, 253, 255);  // #c4b5fd — заголовки розділів
const ImU32 CARD_BG    = IM_COL32(255, 255, 255, 9);
const ImU32 CARD_LINE  = IM_COL32(255, 255, 255, 18);
const ImU32 TEXT       = IM_COL32(228, 228, 231, 255);
const ImU32 TEXT_DIM   = IM_COL32(139, 139, 147, 255);
const ImU32 TITLE_BG   = IM_COL32(23, 20, 31, 255);

const float PAD = 14.0f;        // поля панелі
const float CARD_PAD = 11.0f;   // поля картки
const float TITLE_H = 40.0f;

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void dim_text(const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(TEXT_DIM));
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

void field_label(const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(161, 161, 170, 255)));
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}

// Картка розділу. ImGui не має «рамки навколо того, що зараз намалюю», бо не
// знає наперед висоти, — тож малюємо фон ПІСЛЯ вмісту, у той самий список
// команд, але раніше за нього (ImDrawList приймає зміну порядку через канали).
struct Card {
    ImDrawListSplitter split;
    ImVec2 top;

    void begin(const char* title) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        split.Split(dl, 2);
        split.SetCurrentChannel(dl, 1);          // вміст — у верхньому шарі

        top = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(0, CARD_PAD - 4));
        ImGui::Indent(CARD_PAD);

        ImGui::PushStyleColor(ImGuiCol_Text, col(ACCENT_DIM));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 2));
    }

    void end() {
        ImGui::Unindent(CARD_PAD);
        ImGui::Dummy(ImVec2(0, CARD_PAD - 4));
        const float bottom = ImGui::GetCursorScreenPos().y;
        const float right = top.x + ImGui::GetContentRegionAvail().x;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        split.SetCurrentChannel(dl, 0);          // фон — у нижньому
        dl->AddRectFilled(top, ImVec2(right, bottom), CARD_BG, 10.0f);
        dl->AddRect(top, ImVec2(right, bottom), CARD_LINE, 10.0f);
        split.Merge(dl);
        ImGui::Dummy(ImVec2(0, 8));
    }
};

// Повзунок із підписом справа. Такий самий, як був у Qt: значення поруч, а не
// всередині доріжки, — інакше його не видно на світлій частині.
bool slider_pct(const char* id, float* v, float lo, float hi, const char* fmt) {
    const float label_w = 46.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - label_w - CARD_PAD);
    const bool changed = ImGui::SliderFloat(id, v, lo, hi, "");
    ImGui::SameLine();
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *v);
    ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(233, 213, 255, 255)));
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
    return changed;
}

// Перемикач-тумблер. Стандартна «галочка» ImGui поруч зі скругленими картками
// виглядає як частина іншої програми.
bool toggle(const char* label, bool* on) {
    ImGui::PushID(label);
    const float h = ImGui::GetFrameHeight() * 0.85f;
    const float w = h * 1.8f;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    const bool clicked = ImGui::InvisibleButton("##t", ImVec2(w, h));
    if (clicked) *on = !*on;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 track = *on ? ACCENT
                            : IM_COL32(255, 255, 255, hovered ? 46 : 30);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), track, h * 0.5f);
    const float r = h * 0.5f - 2.0f;
    const float cx = *on ? p.x + w - r - 2.0f : p.x + r + 2.0f;
    dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, IM_COL32(255, 255, 255, 235));

    ImGui::SameLine(0, 8);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return clicked;
}

// Кнопка-привид: без заливки, поки на неї не навели.
bool ghost(const char* label, float width = 0.0f) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.24f));
    const bool r = ImGui::Button(label, ImVec2(width, 0));
    ImGui::PopStyleColor(3);
    return r;
}

void copy_to(char* dst, size_t cap, const std::string& src) {
    const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    memcpy(dst, src.data(), n);
    dst[n] = 0;
}

}  // namespace

void SettingsState::sync(const Config& cfg) {
    copy_to(youtube, sizeof youtube, cfg.youtube);
    copy_to(twitch, sizeof twitch, cfg.twitch);
    copy_to(kick, sizeof kick, cfg.kick);
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
    s.FramePadding = ImVec2(9, 6);
    s.ItemSpacing = ImVec2(8, 7);
    s.ScrollbarRounding = 4.0f;
    s.ScrollbarSize = 9.0f;
    s.GrabMinSize = 12.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = col(IM_COL32(14, 15, 18, 255));
    c[ImGuiCol_Text] = col(TEXT);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.09f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_SliderGrab] = col(ACCENT);
    c[ImGuiCol_SliderGrabActive] = col(IM_COL32(192, 132, 252, 255));
    c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.16f);
    c[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.24f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.18f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.30f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
}

SettingsEvents draw_settings(SettingsState* st, Config* cfg, const std::string& status,
                             const UpdateView& upd, const GameView& game, int w, int h) {
    SettingsEvents ev;
    if (!st->synced) st->sync(*cfg);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::Begin("##settings", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);

    // --- заголовок -----------------------------------------------------------
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + TITLE_H), TITLE_BG);
        dl->AddLine(ImVec2(p.x, p.y + TITLE_H), ImVec2(p.x + w, p.y + TITLE_H),
                    IM_COL32(168, 85, 247, 90));

        ImGui::SetCursorPos(ImVec2(PAD, 11));
        ImGui::TextUnformatted("Налаштування");

        ImGui::SetCursorPos(ImVec2(PAD + 108, 13));
        ImGui::PushStyleColor(ImGuiCol_Text, col(TEXT_DIM));
        ImGui::TextUnformatted(HOMINKA_VERSION);
        ImGui::PopStyleColor();

        // Уся смуга, крім хрестика, — ручка для перетягування.
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##title", ImVec2((float)w - 38.0f, TITLE_H));
        ev.title_active = ImGui::IsItemActive();

        ImGui::SetCursorPos(ImVec2((float)w - 32.0f, 8));
        if (ghost("×", 24.0f)) ev.close = true;
        ImGui::SetCursorPos(ImVec2(0, TITLE_H + 6));
    }

    // --- вміст у прокрутці ---------------------------------------------------
    ImGui::SetCursorPosX(PAD);
    ImGui::BeginChild("##body", ImVec2((float)w - PAD * 2, (float)h - TITLE_H - 12), false);
    const float content_top = ImGui::GetCursorPosY();

    // Канали ------------------------------------------------------------------
    {
        Card card;
        card.begin("КАНАЛИ");

        const float field_w = ImGui::GetContentRegionAvail().x - CARD_PAD;
        field_label("Twitch");
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputTextWithHint("##tw", "twitch.tv/канал або просто нік",
                                     st->twitch, sizeof st->twitch,
                                     ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            cfg->twitch = st->twitch;
            ev.changed = ev.sources_changed = true;
        }

        field_label("Kick");
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputTextWithHint("##kk", "kick.com/канал або просто нік",
                                     st->kick, sizeof st->kick,
                                     ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            cfg->kick = st->kick;
            ev.changed = ev.sources_changed = true;
        }

        field_label("YouTube");
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputTextWithHint("##yt", "@нік, посилання або UC…",
                                     st->youtube, sizeof st->youtube,
                                     ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            cfg->youtube = st->youtube;
            ev.changed = ev.sources_changed = true;
        }

        ImGui::Dummy(ImVec2(0, 2));
        dim_text(status.c_str());
        card.end();
    }

    // Стрічка -----------------------------------------------------------------
    {
        Card card;
        card.begin("СТРІЧКА");

        field_label("Затримка чату");
        float delay = (float)cfg->chat_delay;
        if (slider_pct("##delay", &delay, 0.0f, 60.0f, "%.0f с")) {
            cfg->chat_delay = (int)(delay + 0.5f);
            ev.changed = ev.sources_changed = true;
        }
        dim_text("Тримає повідомлення й видає їх по одному — коли пишуть швидше, "
                 "ніж читаєш, стрічка перестає бути кашею. 0 — без затримки.");
        card.end();
    }

    // Вигляд ------------------------------------------------------------------
    {
        Card card;
        card.begin("ВИГЛЯД");

        field_label("Прозорість вікна");
        float op = cfg->look.opacity * 100.0f;
        if (slider_pct("##op", &op, 25.0f, 100.0f, "%.0f%%")) {
            cfg->look.opacity = op / 100.0f;
            ev.changed = ev.look_changed = true;
        }

        field_label("Тло під чатом");
        float bg = cfg->look.bg_alpha * 100.0f;
        if (slider_pct("##bg", &bg, 0.0f, 100.0f, "%.0f%%")) {
            cfg->look.bg_alpha = bg / 100.0f;
            ev.changed = ev.look_changed = true;
        }

        ImGui::Dummy(ImVec2(0, 2));
        field_label("Розмір тексту");
        if (ghost("A−", 38.0f)) {
            cfg->look.zoom = cfg->look.zoom - 0.1f < 0.5f ? 0.5f : cfg->look.zoom - 0.1f;
            ev.changed = ev.look_changed = true;
        }
        ImGui::SameLine(0, 6);
        {
            char buf[16];
            snprintf(buf, sizeof buf, "%d%%", (int)(cfg->look.zoom * 100.0f + 0.5f));
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(233, 213, 255, 255)));
            ImGui::TextUnformatted(buf);
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(0, 6);
        if (ghost("A+", 38.0f)) {
            cfg->look.zoom = cfg->look.zoom + 0.1f > 3.0f ? 3.0f : cfg->look.zoom + 0.1f;
            ev.changed = ev.look_changed = true;
        }

        ImGui::Dummy(ImVec2(0, 4));
        if (toggle("Без рамки (лише повідомлення)", &cfg->look.frameless))
            ev.changed = ev.look_changed = true;
        dim_text("Прибирає рамку й підкладку — видно самі повідомлення. Керування "
                 "повертається, щойно знімеш замок.");

        ImGui::Dummy(ImVec2(0, 4));
        if (toggle("Тримати вікно поверх усіх", &cfg->keep_top)) ev.changed = true;

        // Свій CSS — окремим вікном: у полі на три сантиметри код не пишуть.
        ImGui::Dummy(ImVec2(0, 6));
        if (ghost("Свій CSS для чату…", ImGui::GetContentRegionAvail().x - CARD_PAD))
            ev.css_editor = true;
        card.end();
    }

    // Чат поверх гри ----------------------------------------------------------
    if (game.supported) {
        Card card;
        card.begin("ЧАТ ПОВЕРХ ГРИ");

        dim_text("Це вікно й так видно поверх майже будь-якої гри — і OBS його не "
                 "знімає. Нижче — те, що потрібно, коли гра забирає екран собі.");

        ImGui::Dummy(ImVec2(0, 6));
        field_label("Гра");
        const float row_w = ImGui::GetContentRegionAvail().x - CARD_PAD;
        ImGui::SetNextItemWidth(row_w - 76.0f);
        const char* current = game.windows.empty()
                                  ? "Немає відкритих ігор — запустіть гру й оновіть"
                                  : game.windows[(size_t)game.picked].c_str();
        if (ImGui::BeginCombo("##game", current)) {
            for (size_t i = 0; i < game.windows.size(); ++i) {
                const bool on = (int)i == game.picked;
                if (ImGui::Selectable(game.windows[i].c_str(), on)) ev.pick_game = (int)i;
                if (on) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 6);
        // Підпис словом, а не значком: стрілка-кільце є не в кожному шрифті,
        // і замість неї виходила порожня плитка.
        if (ghost("Оновити", 70.0f)) ev.refresh_games = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Оновити список вікон");

        ImGui::Dummy(ImVec2(0, 4));
        if (!game.windows.empty()) {
            if (ghost(game.fso_off ? "Повернути повноекранну оптимізацію"
                                   : "Прибрати повноекранну оптимізацію",
                      row_w))
                ev.toggle_fso = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Найнадійніше, коли чат видно в меню, а в бою зникає.\n"
                                  "Діє з наступного запуску гри; у гру нічого не "
                                  "вкладається.");

            const float half = (row_w - 6.0f) / 2.0f;
            if (ghost("Зробити безрамковою", game.restorable ? half : row_w))
                ev.make_borderless = true;
            if (game.restorable) {
                ImGui::SameLine(0, 6);
                if (ghost("Повернути", half)) ev.restore_window = true;
            }
        }

        // Інжект — окремо й наприкінці: це найпотужніше й найризикованіше, і
        // натрапити на нього випадково не можна.
        if (game.injector) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(252, 211, 77, 255)));
            ImGui::TextWrapped("Для досвідчених");
            ImGui::PopStyleColor();
            dim_text("Вкладає бібліотеку в процес гри й малює чат усередині кадру — "
                     "навіть у виключному повноекранному. Потрібне рідко. "
                     "В ОНЛАЙН-іграх з античитом так робити НЕ можна.");
            ImGui::Dummy(ImVec2(0, 4));
            if (!game.windows.empty() &&
                ghost(game.injected ? "Показати ще раз" : "Показати чат у грі", row_w))
                ev.inject = true;
            if (game.injected) {
                ImGui::Dummy(ImVec2(0, 4));
                if (ghost("Прибрати чат із гри", row_w)) ev.stop_inject = true;
                ImGui::Dummy(ImVec2(0, 4));
                field_label("Прозорість чату в грі");
                float op = (float)cfg->game_opacity;
                if (slider_pct("##gop", &op, 30.0f, 255.0f, "%.0f")) {
                    cfg->game_opacity = (int)(op + 0.5f);
                    ev.changed = true;
                }
                if (toggle("Ховати чат від OBS", &cfg->game_hide_obs)) ev.changed = true;
            }
        }

        if (!game.status.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            dim_text(game.status.c_str());
        }
        card.end();
    }

    // Оновлення ---------------------------------------------------------------
    if (upd.supported) {
        Card card;
        card.begin("ОНОВЛЕННЯ");

        field_label("Канал");
        static const char* kIds[] = {"stable", "beta", "dev"};
        static const char* kNames[] = {"Стабільна", "Бета", "Тестова"};
        static const char* kHints[] = {
            "Перевірені випуски. Рекомендовано.",
            "Свіжі можливості до того, як вони потраплять у стабільну.",
            "Збірки одразу після змін. Можуть ламатися.",
        };
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, 4);
            const bool on = cfg->channel == kIds[i];
            ImGui::PushStyleColor(ImGuiCol_Button, on ? col(ACCENT) : ImVec4(1, 1, 1, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  on ? col(ACCENT) : ImVec4(1, 1, 1, 0.16f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(ACCENT));
            if (ImGui::Button(kNames[i])) {
                cfg->channel = kIds[i];
                ev.changed = true;
                ev.check_update = true;   // канал інший — і випуск може бути інший
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kHints[i]);
        }

        ImGui::Dummy(ImVec2(0, 4));
        if (toggle("Перевіряти автоматично", &cfg->auto_update)) ev.changed = true;

        ImGui::Dummy(ImVec2(0, 4));
        if (upd.percent >= 0) {
            ImGui::ProgressBar(upd.percent / 100.0f,
                               ImVec2(ImGui::GetContentRegionAvail().x - CARD_PAD, 6.0f), "");
            ImGui::Dummy(ImVec2(0, 2));
        }
        if (upd.mandatory) {
            ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(252, 211, 77, 255)));
            ImGui::TextWrapped("Це виправлення важливе — краще поставити.");
            ImGui::PopStyleColor();
        }
        dim_text(upd.status.c_str());

        ImGui::Dummy(ImVec2(0, 4));
        const float bw = (ImGui::GetContentRegionAvail().x - CARD_PAD - 6.0f) / 2.0f;
        if (upd.can_check && ghost("Перевірити", bw)) ev.check_update = true;
        if (upd.can_download) {
            if (upd.can_check) ImGui::SameLine(0, 6);
            if (ghost("Завантажити", bw)) ev.start_download = true;
        }
        if (upd.can_install) {
            if (upd.can_check) ImGui::SameLine(0, 6);
            if (ghost("Встановити й перезапустити", bw)) ev.do_install = true;
        }
        card.end();
    }

    // Про програму ------------------------------------------------------------
    {
        Card card;
        card.begin("ПРО ПРОГРАМУ");
        ImGui::TextUnformatted(HOMINKA_NAME " " HOMINKA_VERSION);
        dim_text("Чат читається й малюється самою програмою — без браузера.");
        card.end();
    }

    ev.content_height = (int)(ImGui::GetCursorPosY() - content_top) + (int)TITLE_H + 20;
    ImGui::EndChild();
    ImGui::End();
    return ev;
}

}  // namespace hominka
