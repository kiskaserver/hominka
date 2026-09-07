// Вікно оверлея під X11 — Linux-двійник common/dcomp_window.h.
//
// Що потрібно від вікна чату, і як це дається тут:
//
//   напівпрозоре        — 32-бітний ARGB-візуал. Працює ЛИШЕ коли запущено
//                         композитор (picom, KWin, Mutter…). Без нього X не
//                         змішує альфу, і замість прозорого тла буде чорне —
//                         це не наша поломка, а властивість системи.
//   без рамки й у списку задач його немає
//                       — override-redirect: віконний менеджер такого вікна
//                         не чіпає взагалі.
//   поверх усього       — _NET_WM_STATE_ABOVE плюс сам override-redirect.
//   клік наскрізь       — порожня ВХІДНА область (XShape). Саме вхідна, а не
//                         видима: вікно лишається на екрані, але миша його не
//                         помічає.
//
// Чого тут НЕМАЄ й бути не може: приховування від захоплення. Під Windows це
// один виклик (WDA_EXCLUDEFROMCAPTURE), і завдяки йому чат видно стримеру, але
// не глядачам. В X11 такої речі не існує: будь-яка програма, що знімає екран,
// зніме й наш оверлей. Тому на Linux чат у запису БУДЕ видно, і це треба
// сказати людині, а не мовчати.
#pragma once

#include <X11/Xlib.h>

#include <cstdint>
#include <string>

namespace hominka {

// Що людина зробила з вікном — те саме, що ImGui-рамка під Windows повертає
// через свої кнопки. Поки що зроблено найнеобхідніше: перетягування й розмір.
struct X11Event {
    bool moved = false;               // змінилася геометрія
    bool closed = false;              // вікно закрили
};

class X11Window {
public:
    ~X11Window();

    bool create(int x, int y, int w, int h, const char* title);
    void destroy();
    bool ok() const { return dpy_ != nullptr && win_ != 0; }

    int x() const { return x_; }
    int y() const { return y_; }
    int width() const { return w_; }
    int height() const { return h_; }

    void show();
    void hide();
    bool shown() const { return shown_; }

    void set_geometry(int x, int y, int w, int h);
    // Клік наскрізь: миша перестає помічати вікно.
    void set_click_through(bool on);

    // Кладе на екран готові пікселі (premultiplied BGRA, рядок = w*4).
    void present(const uint8_t* bgra, int w, int h);
    // Прозорим — коли чат вимкнено: вікно лишається, вмісту немає.
    void present_blank();

    // Розбирає події X. Не блокує.
    X11Event poll_events();

private:
    void apply_above();

    Display* dpy_ = nullptr;
    int screen_ = 0;
    Window win_ = 0;
    Visual* visual_ = nullptr;
    Colormap cmap_ = 0;
    GC gc_ = nullptr;
    XImage* image_ = nullptr;         // обгортка над нашим буфером, без копії
    int depth_ = 32;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    bool shown_ = false;
    bool click_through_ = false;
    bool have_shape_ = false;         // чи є розширення XShape
    Atom wm_delete_ = 0;
    // Перетягування: X11 сам такого не робить, тож рахуємо самі.
    bool dragging_ = false;
    int drag_dx_ = 0, drag_dy_ = 0;
    std::string title_;
};

}  // namespace hominka
