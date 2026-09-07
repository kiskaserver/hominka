#include "uifont.h"

#include <initializer_list>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "imgui/imgui.h"

namespace hominka {

namespace {

ImFont* g_mono = nullptr;

#ifdef _WIN32
// Набір символів один на обидва шрифти: кирилиця плюс кілька знаків поза нею.
//
// Українські «ґ», «є», «і», «ї» входять у кириличний набір ImGui, а от решта —
// ні: хрестик закриття, «мінус» у «A−», типографські лапки й тире, якими
// написані підказки. Знаки беремо ті, що в самому шрифті є: «✕» (U+2715),
// наприклад, Segoe UI не має — замість нього вийшла б порожня плитка.
void build_ranges(ImVector<ImWchar>* out) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontGlyphRangesBuilder b;
    b.AddRanges(io.Fonts->GetGlyphRangesDefault());
    b.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    for (ImWchar c : {0x00D7, 0x2212, 0x00AB, 0x00BB, 0x2014, 0x2013, 0x2026,
                      0x2019, 0x25B8, 0x25BE})
        b.AddChar(c);
    b.BuildRanges(out);
}

// Шлях до системного шрифту. Порожньо — якщо теки Windows чомусь немає.
std::string font_path(const char* file) {
    char dir[MAX_PATH] = {0};
    if (!GetWindowsDirectoryA(dir, MAX_PATH)) return "";
    return std::string(dir) + "\\Fonts\\" + file;
}
#endif

}  // namespace

ImFont* mono_font() { return g_mono; }

void load_ui_font(float size) {
#ifdef _WIN32
    const std::string path = font_path("segoeui.ttf");
    if (path.empty()) return;

    // Діапазони треба тримати живими, доки ImGui будує атлас, — а будує він
    // його вже після цього виклику. Локальний вектор зник би одразу.
    static ImVector<ImWchar> kept;
    build_ranges(&kept);
    if (!ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size, nullptr, kept.Data)) {
        // Не знайшовся — лишається вбудований: краще латиниця, ніж жодного
        // інтерфейсу.
        ImGui::GetIO().Fonts->AddFontDefault();
    }
#else
    (void)size;
    ImGui::GetIO().Fonts->AddFontDefault();
#endif
}

// Моноширинний — Consolas, він є в кожній Windows. Не знайшовся — лишається
// nullptr, і редактор малює звичайним шрифтом: гірше, але працює.
void load_mono_font(float size) {
#ifdef _WIN32
    const std::string path = font_path("consola.ttf");
    if (path.empty()) return;
    static ImVector<ImWchar> kept;
    build_ranges(&kept);
    g_mono = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size, nullptr, kept.Data);
#else
    (void)size;
#endif
}

}  // namespace hominka
