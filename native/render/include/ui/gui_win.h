// Звичайне вікно з ImGui — під панель налаштувань.
//
// Чому не всередині вікна чату. Вікно оверлея створене з WS_EX_NOACTIVATE, щоб
// не забирати фокус у гри, — а отже, воно НЕ отримує клавіатури взагалі. У
// налаштуваннях же треба вписувати назви каналів. Тому панель — окреме,
// звичайне вікно, яке фокус брати може.
//
// Друга причина та сама, що була в Qt-версії: налаштування крутять саме тоді,
// коли читають чат, і затуляти його собою — те саме, що правити гучність,
// закривши екран. Тож вікно відкривається ЗБОКУ від чату.
//
// Контекст ImGui тут власний, окремий від того, яким малюється рамка чату. Два
// контексти — звичайна для ImGui річ, треба лише перемикати поточний перед
// кожним викликом, включно з обробником повідомлень.
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <string>
#include <vector>
#include <cstdint>

struct ImGuiContext;

namespace hominka {

class GuiWindow {
public:
    ~GuiWindow();

    // resizable — рамка, за яку вікно тягнеться (редакторові теми це треба,
    // панелі налаштувань — ні: її висоту задає сам вміст).
    // mono — довантажити моноширинний шрифт для коду.
    bool create(const wchar_t* cls, const wchar_t* title, int w, int h,
                bool resizable = false, bool mono = false);
    void destroy();

    // Показує вікно поруч із прямокутником anchor (вікном чату), не вилазячи
    // за край екрана.
    void show_beside(const RECT& anchor);
    void hide();
    bool visible() const { return visible_; }
    // Чи вікно взагалі створено. Створюємо їх ліниво — при першому показі: за
    // кожним стоїть свій пристрій D3D, свій свопчейн і свій атлас шрифтів, а
    // це десятки мегабайтів на вікно, яке людина може жодного разу не
    // відкрити.
    bool created() const { return hwnd_ != nullptr; }

    HWND hwnd() const { return hwnd_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // Кадр: begin() готує ImGui, end() малює й показує. Між ними — сама панель.
    // begin() повертає false, коли вікно сховане: тоді малювати нічого.
    bool begin();
    // present=false лишає намальоване в задньому буфері — саме тоді його можна
    // зняти (у flip-моделі після показу буфер уже інший, і знімок дістав би не
    // той кадр).
    void end(bool present = true);
    void present();

    // Просить у системи саме таку висоту вікна (панель знає, скільки їй треба).
    void want_height(int h);

    // Перетягування за власний заголовок. active — «зараз тримають». Рахуємо
    // від ЕКРАННИХ координат: вікно рухається слідом за курсором, тож віконні
    // на місці й лишалися б, а вікно стояло б як укопане.
    void drag(bool active);

    // Знімок того, що зараз у задньому буфері. Потрібен тому ж, чому й у вікна
    // чату: панель прихована від захоплення екрана, і звичайний скриншот її не
    // бачить — перевірити вигляд інакше нічим.
    // Розмір повертає сам: у вікна з рамкою клієнтська частина менша за саме
    // вікно, і читати буфер за розміром вікна означає вийти за його межі.
    bool capture(std::vector<uint8_t>* bgra, int* w, int* h);

private:
    bool init_gfx(bool mono);
    void release_rtv();
    bool ensure_size(int w, int h);
    static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l);

    HWND hwnd_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    int width_ = 0, height_ = 0;
    bool visible_ = false;
    bool dragging_ = false;
    POINT drag_anchor_ = {0, 0};
    RECT drag_origin_ = {0, 0, 0, 0};
};

}  // namespace hominka
