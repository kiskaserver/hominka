#include "ui/gui_win_sdl.h"

#include <vector>

#include <SDL.h>
#include <SDL_opengl.h>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imgui_impl_sdl2.h"

#include "ui/settings_ui.h"
#include "ui/uifont.h"

namespace hominka {

namespace {

// Усі живі вікна: подія від SDL приходить одна на процес, і роздати її треба
// тому вікну, чий ідентифікатор у ній. Без цього натискання в налаштуваннях
// прилітали б у редактор теми.
std::vector<GuiWindowSDL*>& registry() {
    static std::vector<GuiWindowSDL*> v;
    return v;
}

bool ensure_sdl() {
    static int state = -1;                 // -1 ще не пробували, 0 не вийшло, 1 готово
    if (state >= 0) return state == 1;
    // Тільки відео: ані звук, ані джойстики нам не потрібні, а їхня ініціалізація
    // на машині без звукової карти вміє коштувати секунду.
    state = SDL_Init(SDL_INIT_VIDEO) == 0 ? 1 : 0;
    return state == 1;
}

}  // namespace

GuiWindowSDL::~GuiWindowSDL() { destroy(); }

bool GuiWindowSDL::create(const char* title, int w, int h, bool resizable, bool mono) {
    if (win_) return true;
    if (!ensure_sdl()) return false;

    // Без рамки: заголовок і хрестик малюємо самі — так вікно виглядає однією
    // річчю з чатом, а не гостем із чужої системи. Те саме рішення, що й під
    // Windows.
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN |
                   SDL_WINDOW_ALLOW_HIGHDPI;
    if (resizable) flags |= SDL_WINDOW_RESIZABLE;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    win_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                            flags);
    if (!win_) return false;
    gl_ = SDL_GL_CreateContext(win_);
    if (!gl_) {
        SDL_DestroyWindow(win_);
        win_ = nullptr;
        return false;
    }
    SDL_GL_MakeCurrent(win_, (SDL_GLContext)gl_);
    SDL_GL_SetSwapInterval(1);

    id_ = SDL_GetWindowID(win_);
    width_ = w;
    height_ = h;

    IMGUI_CHECKVERSION();
    imgui_ = ImGui::CreateContext();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;      // програма не лишає файлів там, звідки її запустили
    io.LogFilename = nullptr;
    load_ui_font(19.0f);
    if (mono) load_mono_font(18.0f);
    settings_style();
    const bool ok = ImGui_ImplSDL2_InitForOpenGL(win_, gl_) &&
                    ImGui_ImplOpenGL3_Init("#version 130");
    ImGui::SetCurrentContext(prev);
    if (!ok) {
        destroy();
        return false;
    }
    registry().push_back(this);
    return true;
}

void GuiWindowSDL::destroy() {
    for (size_t i = 0; i < registry().size(); ++i)
        if (registry()[i] == this) {
            registry().erase(registry().begin() + i);
            break;
        }
    if (imgui_) {
        ImGuiContext* mine = imgui_;
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(mine);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(mine);
        imgui_ = nullptr;
        // Повертаємо чужий контекст — але лише якщо поточним був не наш: інакше
        // ми поставили б указівник на щойно знищене.
        ImGui::SetCurrentContext(prev == mine ? nullptr : prev);
    }
    if (gl_) {
        SDL_GL_DeleteContext((SDL_GLContext)gl_);
        gl_ = nullptr;
    }
    if (win_) {
        SDL_DestroyWindow(win_);
        win_ = nullptr;
    }
    visible_ = false;
}

void GuiWindowSDL::pump() {
    if (registry().empty()) return;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        // Кому подія. У подій миші й клавіатури ідентифікатор вікна лежить у
        // різних полях, тож питаємо SDL напряму там, де можемо.
        unsigned id = 0;
        switch (ev.type) {
        case SDL_WINDOWEVENT: id = ev.window.windowID; break;
        case SDL_MOUSEMOTION: id = ev.motion.windowID; break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: id = ev.button.windowID; break;
        case SDL_MOUSEWHEEL: id = ev.wheel.windowID; break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: id = ev.key.windowID; break;
        case SDL_TEXTINPUT: id = ev.text.windowID; break;
        default: id = 0; break;
        }
        for (GuiWindowSDL* w : registry()) {
            if (id && w->id_ != id) continue;
            ImGuiContext* prev = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(w->imgui_);
            ImGui_ImplSDL2_ProcessEvent(&ev);
            ImGui::SetCurrentContext(prev);
            if (ev.type == SDL_WINDOWEVENT && id == w->id_) {
                if (ev.window.event == SDL_WINDOWEVENT_CLOSE) w->hide();
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    w->width_ = ev.window.data1;
                    w->height_ = ev.window.data2;
                }
            }
            if (id) break;
        }
    }
}

void GuiWindowSDL::show_beside(int ax, int ay, int aw, int ah) {
    if (!win_) return;
    SDL_Rect screen = {0, 0, 1920, 1080};
    SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(win_), &screen);

    const int gap = 12;
    int x = ax + aw + gap;
    if (x + width_ > screen.x + screen.w) x = ax - width_ - gap;   // не влізло — ліворуч
    if (x < screen.x) x = screen.x + gap;
    int y = ay;
    if (y + height_ > screen.y + screen.h) y = screen.y + screen.h - height_ - gap;
    if (y < screen.y) y = screen.y + gap;

    SDL_SetWindowPosition(win_, x, y);
    SDL_ShowWindow(win_);
    SDL_RaiseWindow(win_);
    visible_ = true;
}

void GuiWindowSDL::hide() {
    if (!win_) return;
    SDL_HideWindow(win_);
    visible_ = false;
}

void GuiWindowSDL::want_height(int h) {
    if (!win_ || h <= 0 || h == height_) return;
    SDL_Rect screen = {0, 0, 1920, 1080};
    SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(win_), &screen);
    const int avail = screen.h - 24;
    if (h > avail) h = avail;
    SDL_SetWindowSize(win_, width_, h);
    height_ = h;
}

void GuiWindowSDL::drag(bool active) {
    if (!win_) return;
    if (!active) { dragging_ = false; return; }
    int mx = 0, my = 0;
    SDL_GetGlobalMouseState(&mx, &my);
    if (!dragging_) {
        dragging_ = true;
        drag_ax_ = mx;
        drag_ay_ = my;
        SDL_GetWindowPosition(win_, &drag_wx_, &drag_wy_);
        return;
    }
    SDL_SetWindowPosition(win_, drag_wx_ + (mx - drag_ax_), drag_wy_ + (my - drag_ay_));
}

bool GuiWindowSDL::begin() {
    if (!visible_ || !imgui_ || !win_) return false;
    SDL_GL_MakeCurrent(win_, (SDL_GLContext)gl_);
    ImGui::SetCurrentContext(imgui_);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    return true;
}

void GuiWindowSDL::end() {
    if (!imgui_ || !win_) return;
    ImGui::Render();
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(win_, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.055f, 0.059f, 0.070f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(win_);
}

}  // namespace hominka
