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

#include <cstdint>
#include <memory>
#include <string>

namespace hominka {

// Що сталося з вікном за це коло.
//
// Миша віддається НАЗОВНІ, а не обробляється тут, і це навмисно: тільки рамка
// знає, чи потрапило натискання в кнопку, у смужку чи в порожнє місце. Вікно
// вміє лише те, чого рамка не вміє, — рухати й розтягувати себе.
struct X11Event {
    bool moved = false;               // змінилася геометрія
    bool closed = false;              // вікно закрили
    bool motion = false;              // курсор рухався
    bool press = false;               // натиснули ліву
    bool release = false;             // відпустили ліву
    bool leave = false;               // курсор пішов з вікна
    bool hotkey = false;              // натиснули Ctrl+Alt+Space
    int mx = 0, my = 0;               // курсор у координатах вікна
};

class X11Window {
public:
    X11Window();
    ~X11Window();

    bool create(int x, int y, int w, int h, const char* title);
    void destroy();
    bool ok() const;

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

    // Перехоплює Ctrl+Alt+Space на весь екран — той самий замок, що й у
    // смужці. Потрібне саме глобальне: вікно чату фокус не бере (інакше
    // забирало б його в гри), тож звичайних натискань воно не бачить.
    void grab_hotkey();

    // Кладе на екран готові пікселі (premultiplied BGRA, рядок = w*4).
    void present(const uint8_t* bgra, int w, int h);
    // Прозорим — коли чат вимкнено: вікно лишається, вмісту немає.
    void present_blank();

    // Наступна подія X, або false, якщо черга порожня. Не блокує.
    //
    // Саме ПО ОДНІЙ, а не пачкою. Пачкою було так: натискання й перші рухи
    // миші приходять разом, рухи обробляються ще до того, як рамка вирішила
    // «це драг», — і перетягування або втрачало початок, або не починалося
    // зовсім. Хто розбирає події, той і має вирішувати після кожної.
    bool poll_event(X11Event* out);

    // Почати тягнути вікно (за смужку) або розтягувати (за куточок). Далі рух
    // веде саме вікно, доки не відпустять: X11 такого не робить, віконного
    // менеджера в override-redirect вікна немає.
    void start_drag(int mx, int my);
    void start_resize(int mx, int my);
    void end_drag();
    bool dragging() const { return dragging_ || resizing_; }

private:
    void apply_above();

    // Усе, що від X11, — за вказівником і лише в .cpp.
    //
    // Не заради краси: <X11/Xlib.h> оголошує макроси зі звичайнісінькими
    // іменами — None, Bool, Status, Success. Заголовок, який їх приносить,
    // отруює КОЖЕН файл, що його включив: «enum class Hit { None, … }» у
    // рамці вікна перестає збиратися, і повідомлення про помилку вказує на
    // рамку, а не на того, хто приніс макрос. Одного разу ми на це вже
    // наступили.
    struct Impl;
    std::unique_ptr<Impl> p_;

    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    bool shown_ = false;
    bool click_through_ = false;
    bool have_shape_ = false;         // чи є розширення XShape
    // Перетягування й розтягування: X11 сам такого не робить, рахуємо самі.
    bool dragging_ = false;
    bool resizing_ = false;
    int drag_dx_ = 0, drag_dy_ = 0;
    int resize_w_ = 0, resize_h_ = 0;
    std::string title_;
};

}  // namespace hominka
