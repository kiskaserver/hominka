#include "uifont.h"

#include <initializer_list>

#ifdef _WIN32
#include <windows.h>
#endif

#include "imgui/imgui.h"

namespace hominka {

void load_ui_font(float size) {
    ImGuiIO& io = ImGui::GetIO();

#ifdef _WIN32
    char path[MAX_PATH] = {0};
    if (!GetWindowsDirectoryA(path, MAX_PATH)) return;
    lstrcatA(path, "\\Fonts\\segoeui.ttf");

    ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder b;
    b.AddRanges(io.Fonts->GetGlyphRangesDefault());
    b.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    // Українські «ґ», «є», «і», «ї» входять у кириличний набір ImGui, а от це —
    // ні: хрестик закриття, «мінус» у «A−», типографські лапки й тире, якими
    // написані підказки. Знаки беремо ті, що є в самому Segoe UI: «✕» (U+2715),
    // наприклад, там відсутній — замість нього виходить порожня плитка.
    for (ImWchar c : {0x00D7, 0x2212, 0x00AB, 0x00BB, 0x2014, 0x2013, 0x2026,
                      0x2019, 0x25B8, 0x25BE})
        b.AddChar(c);
    b.BuildRanges(&ranges);

    // Шрифт треба тримати живим, доки ImGui будує атлас, — статичний вектор
    // саме для цього: локальний зник би одразу після виходу.
    static ImVector<ImWchar> kept;
    kept = ranges;
    if (!io.Fonts->AddFontFromFileTTF(path, size, nullptr, kept.Data)) {
        // Не знайшовся — лишається вбудований: краще латиниця, ніж жодного
        // інтерфейсу.
        io.Fonts->AddFontDefault();
    }
#else
    (void)size;
    io.Fonts->AddFontDefault();
#endif
}

}  // namespace hominka
