// Рамка вікна чату: підкладка, смужка перетягування, замок, шестерня,
// повзунки й куточок для розтягування.
//
// Це те саме, що робили hominka/ui/chrome.py і частина панелі налаштувань, —
// але вже в самому вікні оверлея. Причина проста: на етапі 2 вікно чату більше
// не Qt-вікно, а це, нативне. Отже, і крутити його має воно.
//
// Чому Dear ImGui. Малювати кнопки й повзунки «руками» на Direct2D означало б
// писати обробку наведення, натискання, захоплення миші й повторюваності — тобто
// цілий інтерфейсний рушій заради семи елементів керування. ImGui це вже вміє,
// важить кілька файлів і не тягне жодної залежності.
//
// Розподіл праці з Direct2D: підкладку й рамку малює D2D (там скруглення й
// прозорість того самого штибу, що й у чаті), а все, з чим взаємодіють мишею, —
// ImGui поверх. Обидва пишуть у той самий задній буфер, між ними — EndDraw.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>

#include <string>

namespace hominka {

// Вигляд вікна — те, що людина крутить постійно. Значення ті самі, що в
// config.json (opacity, bg_alpha, zoom), і назви навмисно ті самі.
struct Look {
    float opacity = 0.94f;      // прозорість усього вікна
    float bg_alpha = 0.30f;     // прозорість підкладки під чатом
    float zoom = 1.0f;          // кегль тексту
    bool frameless = false;     // «лише повідомлення»: без підкладки й рамки
    bool locked = false;        // клік-крізь: вікно не заважає ані грі, ані столу
};

// Що людина щойно зробила. Рендер збирає це за кадр і віддає Python, щоб той
// зберіг у config.json — правда про налаштування лишається в одному місці.
struct ChromeEvents {
    bool look_changed = false;      // покрутили повзунок або натиснули A+/A−
    bool lock_changed = false;
    bool geometry_changed = false;  // перетягнули або розтягнули
    bool open_settings = false;     // натиснули шестерню
    bool close = false;             // натиснули хрестик
};

class Chrome {
public:
    bool init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void shutdown();

    // Обробник повідомлень вікна — ставиться в DCompWindow::set_msg_hook.
    static LRESULT msg_hook(HWND h, UINT m, WPARAM w, LPARAM l, bool* handled);

    // Чи миша зараз над вікном. Поки її немає, рамку не показуємо взагалі:
    // поверх гри постійна смужка — це шум, а не зручність.
    bool hovered() const { return hovered_; }

    // Підкладка й рамка — малює Direct2D, ДО чату.
    void draw_backdrop(ID2D1DeviceContext* d2d, int w, int h, const Look& look) const;

    // Елементи керування — малює ImGui, ПІСЛЯ чату.
    // Повертає, що людина зробила; look міняється на місці.
    ChromeEvents draw_controls(int w, int h, Look* look, HWND hwnd);

    // Чи ImGui зараз хоче мишу (курсор над кнопкою, тягнуть повзунок). Поки
    // хоче — вікно не можна робити клік-крізь, інакше натискання провалиться.
    bool wants_mouse() const;

private:
    bool ready_ = false;
    bool hovered_ = false;
    // Перетягування за смужку: тримаємо, звідки взялися, щоб вікно не
    // «стрибало» під курсор при першому русі.
    bool dragging_ = false;
    POINT drag_anchor_ = {0, 0};
    RECT drag_origin_ = {0, 0, 0, 0};
    bool resizing_ = false;
    POINT resize_anchor_ = {0, 0};
    int resize_w_ = 0, resize_h_ = 0;
};

}  // namespace hominka
