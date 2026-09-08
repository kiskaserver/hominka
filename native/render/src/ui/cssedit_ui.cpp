#include "ui/cssedit_ui.h"

#include <cstdio>
#include <vector>

#include "ui/csslint.h"
#include "ui/cssref.h"
#include "imgui/imgui.h"
#include "ui/uibits.h"
#include "ui/uifont.h"

namespace hominka {

namespace {

const ImU32 ACCENT     = IM_COL32(168, 85, 247, 255);
const ImU32 ACCENT_DIM = IM_COL32(196, 181, 253, 255);
const ImU32 TEXT_DIM   = IM_COL32(139, 139, 147, 255);
const ImU32 TITLE_BG   = IM_COL32(23, 20, 31, 255);
const ImU32 ERR        = IM_COL32(252, 165, 165, 255);   // #fca5a5
const ImU32 WARN       = IM_COL32(252, 211, 77, 255);    // #fcd34d

const float PAD = 12.0f;
const float TITLE_H = 40.0f;
// Скільки чекати після останньої натиснутої клавіші, перш ніж застосувати.
// Застосовувати щолітеру означало б перекладати всю стрічку на кожен символ.
const int64_t APPLY_DELAY_MS = 350;

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void dim(const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(TEXT_DIM));
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

// ImGui працює з голим буфером, а нам зручніше зі std::string. Зворотний
// виклик росте разом із текстом — інакше тема впиралася б у стелю, яку ми
// вигадали наперед. Це той самий прийом, що в imgui_stdlib.cpp.
int text_resize(ImGuiInputTextCallbackData* data) {
    CssEditState* st = (CssEditState*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        st->text.resize((size_t)data->BufTextLen);
        data->Buf = (char*)st->text.c_str();
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        // Рядок, у якому зараз курсор: за ним людина знаходить місце, на яке
        // свариться список проблем.
        int line = 1;
        for (int i = 0; i < data->CursorPos && i < data->BufTextLen; ++i)
            if (data->Buf[i] == '\n') ++line;
        st->cursor_line = line;
    }
    return 0;
}

// Вставляє шматок у кінець тексту. Саме в кінець, а не «під курсор»: правило,
// дописане останнім, перебиває попередні — тобто робить те, чого людина й
// чекає від «вставити рецепт».
void append_code(CssEditState* st, const char* code) {
    if (!st->text.empty() && st->text.back() != '\n') st->text += '\n';
    if (!st->text.empty()) st->text += '\n';
    st->text += code;
    st->text += '\n';
    st->pending = true;
}

void draw_problems(const std::string& text) {
    const std::vector<CssProblem> errors = validate_css(text);
    const std::vector<CssProblem> warns = lint_css(text);

    if (errors.empty() && warns.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(IM_COL32(134, 239, 172, 255)));
        ImGui::TextWrapped("Помилок немає, і все, що написано, рушій уміє.");
        ImGui::PopStyleColor();
        return;
    }

    if (!errors.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(ERR));
        ImGui::Text("Помилки (%d) — правило зникає цілком", (int)errors.size());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        for (const CssProblem& p : errors) {
            ImGui::PushStyleColor(ImGuiCol_Text, col(ERR));
            ImGui::Text("рядок %d", p.line);
            ImGui::PopStyleColor();
            ImGui::SameLine(72.0f);
            ImGui::TextWrapped("%s", p.text.c_str());
            ImGui::Spacing();
        }
        ImGui::Separator();
        ImGui::Spacing();
    }

    if (!warns.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(WARN));
        ImGui::Text("Рушій цього не вміє (%d)", (int)warns.size());
        ImGui::PopStyleColor();
        dim("Верстка не зламається — ці правила просто нічого не зроблять.");
        ImGui::Spacing();
        for (const CssProblem& p : warns) {
            ImGui::PushStyleColor(ImGuiCol_Text, col(WARN));
            ImGui::Text("рядок %d", p.line);
            ImGui::PopStyleColor();
            ImGui::SameLine(72.0f);
            ImGui::TextWrapped("%s", p.text.c_str());
            ImGui::Spacing();
        }
    }
}

void draw_recipes(CssEditState* st) {
    dim("Подвійний клац — дописати в кінець теми.");
    ImGui::Spacing();
    for (const CssRecipe& r : css_recipes()) {
        ImGui::PushID(r.name);
        ImGui::Selectable(r.name, false, ImGuiSelectableFlags_AllowDoubleClick);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) append_code(st, r.code);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n\n%s", r.hint, r.code);
        ImGui::PushStyleColor(ImGuiCol_Text, col(TEXT_DIM));
        ImGui::TextWrapped("%s", r.hint);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PopID();
    }
}

void draw_reference(CssEditState* st) {
    dim("Подвійний клац — дописати приклад у кінець теми.");
    ImGui::Spacing();
    for (const CssSelector& s : css_selectors()) {
        ImGui::PushID(s.sel);
        ImGui::PushStyleColor(ImGuiCol_Text, col(ACCENT_DIM));
        ImGui::Selectable(s.sel, false, ImGuiSelectableFlags_AllowDoubleClick);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) append_code(st, s.code);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.code);
        ImGui::TextWrapped("%s", s.title);
        ImGui::PushStyleColor(ImGuiCol_Text, col(TEXT_DIM));
        ImGui::TextWrapped("%s", s.what);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PopID();
    }
}

}  // namespace

CssEditEvents draw_css_editor(CssEditState* st, int w, int h, int64_t now_ms) {
    CssEditEvents ev;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::Begin("##css", nullptr,
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
        ImGui::TextUnformatted("Свій CSS для чату");

        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::InvisibleButton("##title", ImVec2((float)w - 468.0f, TITLE_H));
        ev.title_active = ImGui::IsItemActive();

        ImGui::SetCursorPos(ImVec2((float)w - 460.0f, 7));
        if (ghost(st->samples_on ? "Прибрати зразки" : "Показати зразки", 150.0f))
            ev.samples = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(st->samples_on
                                  ? "Зразки зникнуть зі стрічки. Вони зникають і самі,\n"
                                    "коли закрити це вікно."
                                  : "Приклади повідомлень у самій стрічці, по одному —\n"
                                    "щоб бачити тему в русі, коли чат мовчить.");
        ImGui::SameLine(0, 6);
        ImGui::SetCursorPosY(7);
        // Скидання — у два кроки. Перший клац лише перепитує; за п'ять секунд
        // питання знімається саме́. Свій CSS пишуть годинами, і одного
        // випадкового кліка для його втрати замало.
        const bool asked = st->reset_asked_ms && now_ms - st->reset_asked_ms < 5000;
        if (asked) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.94f, 0.27f, 0.27f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.94f, 0.27f, 0.27f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.86f, 0.15f, 0.15f, 1.0f));
        }
        if (ImGui::Button(asked ? "Точно скинути?" : "Скинути до типових",
                          ImVec2(asked ? 146.0f : 146.0f, 0))) {
            if (asked) {
                ev.reset = true;
                st->reset_asked_ms = 0;
            } else {
                st->reset_asked_ms = now_ms;
            }
        }
        if (asked) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Прибрати свій CSS — лишиться наше типове оформлення.");

        ImGui::SameLine(0, 6);
        ImGui::SetCursorPosY(7);
        if (ghost("Застосувати", 104.0f)) { ev.apply = true; st->pending = false; }
        if (close_button((float)w - 38.0f, (TITLE_H - 30.0f) * 0.5f, 30.0f)) ev.close = true;
        ImGui::SetCursorPos(ImVec2(0, TITLE_H + 6));
    }

    const float body_h = (float)h - TITLE_H - PAD - 26.0f;
    const float left_w = (float)w * 0.56f;

    // --- поле з темою --------------------------------------------------------
    ImGui::SetCursorPosX(PAD);
    ImGui::BeginChild("##left", ImVec2(left_w, body_h), false);
    {
        if (!st->text.empty() && st->text.back() != '\0') st->text.push_back('\0');
        if (st->text.empty()) st->text.push_back('\0');

        ImFont* mono = mono_font();
        if (mono) ImGui::PushFont(mono);
        const bool changed = ImGui::InputTextMultiline(
            "##css_text", (char*)st->text.c_str(), st->text.capacity() + 1,
            ImVec2(left_w - PAD, body_h - 24.0f),
            ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize |
                ImGuiInputTextFlags_CallbackAlways,
            text_resize, st);
        if (mono) ImGui::PopFont();

        if (changed) {
            st->typed_ms = now_ms;
            st->pending = true;
        }
        // Застосовуємо самі, коли пальці зупинилися: людина не має шукати
        // кнопку, щоб побачити, що вийшло.
        if (st->pending && now_ms - st->typed_ms > APPLY_DELAY_MS) {
            st->pending = false;
            ev.apply = true;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, col(TEXT_DIM));
        ImGui::Text("рядок %d   ·   %d символів   ·   правки лягають у чат самі",
                    st->cursor_line, (int)st->text.size());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // --- проблеми, рецепти, довідник ----------------------------------------
    ImGui::SameLine(0, PAD);
    ImGui::BeginChild("##right", ImVec2(ImGui::GetContentRegionAvail().x - PAD, body_h), false);
    {
        const char* tabs[] = {"Проблеми", "Рецепти", "Що можна стилізувати"};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, 4);
            const bool on = st->tab == i;
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  on ? col(ACCENT) : ImVec4(1, 1, 1, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  on ? col(ACCENT) : ImVec4(1, 1, 1, 0.16f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(ACCENT));
            if (ImGui::Button(tabs[i])) st->tab = i;
            ImGui::PopStyleColor(3);
        }
        ImGui::Spacing();
        // Без рамки: стандартна ImGui-обводка тут світло-сіра, і навколо
        // «Проблем» виходив чужий білий прямокутник. Замість неї — ледь
        // світліше тло, того самого роду, що й решта карток.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.03f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::BeginChild("##tabbody", ImVec2(0, 0), false);
        if (st->tab == 0) draw_problems(st->text);
        else if (st->tab == 1) draw_recipes(st);
        else draw_reference(st);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::End();
    return ev;
}

}  // namespace hominka
