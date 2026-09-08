#include "ui/uifont.h"

#include <initializer_list>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fontconfig/fontconfig.h>
#endif

#include "imgui/imgui.h"

namespace hominka {

namespace {

ImFont* g_mono = nullptr;

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

// Як растеризувати гліфи.
//
// Растеризатор — FreeType (вмикається в imconfig.h під час збірки ImGui), і це
// головне: вбудований stb_truetype НЕ виконує хінтинг, тобто ігнорує вказівки
// самого шрифту, як укласти лінії літери на пікселі. Segoe UI розрахований
// саме на них; без хінтингу його стовпчики лягають між пікселями, і на
// п'ятнадцяти пікселях літери виходять рвані й різної товщини. Поруч із чатом,
// який малює DirectWrite (а він хінтинг виконує), це видно одразу.
//
// Прапорців НЕ ставимо навмисно: типовий режим FreeType — рідний хінтер
// шрифту, тобто рівно те, чим користується сама Windows. LightHinting дає
// м'якші літери й ближчий до ClearType вигляд, але тут потрібна саме різкість.
//
// Наддискретизація при цьому зайва: вона розмиває те, що хінтинг щойно
// вирівняв по пікселях.
ImFontConfig& sharp() {
    static ImFontConfig cfg;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    // Відступ кожного гліфа — ціле число пікселів. Інакше всередині рядка вони
    // поступово з'їжджають на дробові позиції, і хінтинг знову ні до чого.
    cfg.PixelSnapH = true;
    return cfg;
}

#ifdef _WIN32
// Шлях до системного шрифту. Порожньо — якщо теки Windows чомусь немає.
std::string font_path(const char* file) {
    char dir[MAX_PATH] = {0};
    if (!GetWindowsDirectoryA(dir, MAX_PATH)) return "";
    return std::string(dir) + "\\Fonts\\" + file;
}
#else
// Файл шрифту за назвою родини.
//
// Того самого fontconfig питає й розкладка чату (gfx/fontstore.cpp) — тож
// інтерфейс і чат беруть шрифти з одного джерела. Якщо просимої родини немає,
// fontconfig віддає найближчу, і це саме та поведінка, якої тут хочеться:
// краще чужий шрифт, ніж порожні плитки замість літер.
std::string fc_file(const char* family) {
    if (!FcInit()) return "";
    FcPattern* pat = FcPatternCreate();
    if (!pat) return "";
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)family);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern* got = FcFontMatch(nullptr, pat, &res);
    FcPatternDestroy(pat);
    if (!got) return "";
    std::string out;
    FcChar8* file = nullptr;
    if (FcPatternGetString(got, FC_FILE, 0, &file) == FcResultMatch && file)
        out = (const char*)file;
    FcPatternDestroy(got);
    return out;
}
#endif

}  // namespace

ImFont* mono_font() { return g_mono; }

void load_ui_font(float size) {
#ifdef _WIN32
    const std::string path = font_path("segoeui.ttf");
#else
    const std::string path = fc_file("DejaVu Sans");
#endif
    // Діапазони треба тримати живими, доки ImGui будує атлас, — а будує він
    // його вже після цього виклику. Локальний вектор зник би одразу.
    static ImVector<ImWchar> kept;
    build_ranges(&kept);
    if (path.empty() ||
        !ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size, &sharp(), kept.Data)) {
        // Не знайшовся — лишається вбудований: краще латиниця, ніж жодного
        // інтерфейсу.
        ImGui::GetIO().Fonts->AddFontDefault();
    }
}

// Моноширинний — для коду в редакторі теми. Не знайшовся — лишається nullptr,
// і редактор малює звичайним шрифтом: гірше, але працює.
void load_mono_font(float size) {
#ifdef _WIN32
    const std::string path = font_path("consola.ttf");
#else
    const std::string path = fc_file("DejaVu Sans Mono");
#endif
    if (path.empty()) return;
    static ImVector<ImWchar> kept;
    build_ranges(&kept);
    g_mono = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size, &sharp(), kept.Data);
}

}  // namespace hominka
