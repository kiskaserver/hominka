// Звичайне вікно з ImGui під Linux — те саме, що ui/gui_win.h під Windows.
//
// Навіщо взагалі окреме вікно, а не куточок у вікні чату: вікно чату не бере
// фокус (інакше воно забирало б його в гри), а отже й клавіатури не отримує —
// а в налаштуваннях треба вписувати назви каналів. І друге: налаштування
// крутять, дивлячись у чат, тож затуляти його собою не можна.
//
// Чому SDL2. Панелі малює Dear ImGui, а йому потрібен хтось, хто відкриє вікно
// й дасть події. Офіційного бекенда під голий X11 у ImGui немає — є під SDL2, і
// саме з ним він живе найдовше. SDL2 вкладений статично, тож нових бібліотек
// у системі це не вимагає.
//
// Файли панелей (ui/settings_ui.cpp, ui/cssedit_ui.cpp) спільні з Windows і
// нічого про це вікно не знають — вони чистий ImGui.
#pragma once

#include <string>

struct ImGuiContext;
struct SDL_Window;

namespace hominka {

class GuiWindowSDL {
public:
    ~GuiWindowSDL();

    // resizable — вікно можна тягнути за краї (редакторові теми це треба).
    // mono — довантажити моноширинний шрифт для коду.
    bool create(const char* title, int w, int h, bool resizable = false,
                bool mono = false);
    void destroy();

    // Показує вікно поруч із прямокутником вікна чату, не вилазячи за край
    // екрана. Поверх чату не кладемо ніколи.
    void show_beside(int ax, int ay, int aw, int ah);
    void hide();
    bool visible() const { return visible_; }
    // Створюємо ліниво, при першому показі: за кожним вікном стоїть свій
    // контекст OpenGL і свій атлас шрифтів.
    bool created() const { return win_ != nullptr; }

    int width() const { return width_; }
    int height() const { return height_; }

    // Кадр: begin() готує ImGui, end() малює й показує.
    bool begin();
    void end();

    // Просить у системи саме таку висоту (панель знає, скільки їй треба).
    void want_height(int h);

    // Перетягування за власний заголовок: рахуємо від ЕКРАННИХ координат —
    // вікно рухається слідом за курсором, тож віконні на місці й лишалися б.
    void drag(bool active);

    // Події SDL спільні на весь процес, тож розбирає їх ОДИН виклик і роздає
    // тому вікну, чий ідентифікатор у події. Кличеться раз на кадр, до begin().
    static void pump();

private:
    SDL_Window* win_ = nullptr;
    void* gl_ = nullptr;                 // SDL_GLContext
    ImGuiContext* imgui_ = nullptr;
    unsigned id_ = 0;                    // SDL_GetWindowID
    int width_ = 0, height_ = 0;
    bool visible_ = false;
    bool dragging_ = false;
    int drag_ax_ = 0, drag_ay_ = 0;      // курсор на початку перетягування
    int drag_wx_ = 0, drag_wy_ = 0;      // вікно на початку перетягування
};

}  // namespace hominka
