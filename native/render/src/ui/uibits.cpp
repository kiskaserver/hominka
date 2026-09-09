#include "ui/uibits.h"

#include <cmath>

#include "imgui/imgui.h"

namespace hominka {

void text_at(ImDrawList* dl, float x, float y, unsigned int color,
             const char* text) {
    // floorf, а не IM_FLOOR: той макрос внутрішній для ImGui й у публічному
    // заголовку його немає.
    if (dl) dl->AddText(ImVec2(floorf(x), floorf(y)), color, text);
}

bool ghost(const char* label, float width) {
    // width — мінімум, а не догма. Ширина кнопок задана числами, і напис,
    // який у неї не вліз, ImGui просто обрізає: досить було підняти шрифт на
    // піксель, щоб «Увімкнути повноекранну оптимізацію» втратило хвіст.
    const float need =
        ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (need > width) width = need;

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.24f));
    const bool r = ImGui::Button(label, ImVec2(width, 0));
    ImGui::PopStyleColor(3);
    return r;
}

bool close_button(float x, float y, float size) {
    ImGui::SetCursorPos(ImVec2(x, y));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##close", ImVec2(size, size));
    const bool hot = ImGui::IsItemHovered();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hot)
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size),
                          ImGui::IsItemActive() ? IM_COL32(220, 38, 38, 255)
                                                : IM_COL32(239, 68, 68, 235),
                          7.0f);
    const ImU32 tint = hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(139, 139, 147, 255);
    const float r = size * 0.17f;
    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), tint, 1.7f);
    dl->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), tint, 1.7f);
    return pressed;
}

void app_logo(ImDrawList* dl, float x, float y, float s) {
    const ImVec2 p(x, y);
    dl->AddRectFilledMultiColor(p, ImVec2(x + s, y + s),
                                IM_COL32(150, 90, 240, 255), IM_COL32(214, 70, 190, 255),
                                IM_COL32(214, 70, 190, 255), IM_COL32(150, 90, 240, 255));
    // Скруглення підробляємо зверху: AddRectFilledMultiColor кутів не вміє.
    dl->AddRect(p, ImVec2(x + s, y + s), IM_COL32(0, 0, 0, 0), s * 0.28f, 0, 0.0f);

    const float bx = x + s * 0.17f, by = y + s * 0.25f;
    const float bw = s * 0.66f, bh = s * 0.42f;
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), IM_COL32(255, 255, 255, 245),
                      bh * 0.42f);
    // Хвостик бульбашки.
    dl->AddTriangleFilled(ImVec2(bx + bw * 0.22f, by + bh - 0.5f),
                          ImVec2(bx + bw * 0.52f, by + bh - 0.5f),
                          ImVec2(bx + bw * 0.24f, by + bh + s * 0.22f),
                          IM_COL32(255, 255, 255, 245));
    const ImU32 dots[3] = {IM_COL32(99, 102, 241, 255), IM_COL32(147, 51, 234, 255),
                           IM_COL32(236, 72, 153, 255)};
    for (int i = 0; i < 3; ++i)
        dl->AddCircleFilled(ImVec2(bx + bw * (0.26f + 0.24f * (float)i), by + bh * 0.5f),
                            s * 0.055f + 0.4f, dots[i], 8);
}

}  // namespace hominka
