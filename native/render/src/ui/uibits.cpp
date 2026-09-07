#include "ui/uibits.h"

#include "imgui/imgui.h"

namespace hominka {

bool ghost(const char* label, float width) {
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

}  // namespace hominka
