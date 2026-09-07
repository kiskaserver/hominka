// Рамка вікна чату під Linux: підкладка, смужка, замок, кегль, прозорість,
// шестерня, куточок для розтягування.
//
// Чому не Dear ImGui, як під Windows. ImGui віддає списки трикутників із
// текстурою шрифтового атласу, а Blend2D такого не малює: у нього шляхи,
// картинки й патерни, але не растеризація трикутників із поперсинними
// координатами текстури. Робити для нього бекенд ImGui означало б писати
// растеризатор — заради семи елементів керування.
//
// Тому тут вони намальовані прямо: прямокутники, скруглення й короткі написи
// гліфами з FontStore. Обробка наведення й натискання — теж своя, але це
// півсотні рядків, бо станів рівно два (наведено, натиснуто).
//
// Що НЕ дублюється: набір значень (Look) і набір подій (ChromeEvents) спільні
// з віконною рамкою — див. look.h. Python на тому боці розбирає один випадок.
#pragma once

#include <blend2d.h>

#include <string>

#include "fontstore.h"
#include "look.h"

namespace hominka {

class ChromeBL {
public:
    void set_fonts(FontStore* f) { fonts_ = f; }

    // --- події миші від X11 ------------------------------------------------
    void on_motion(int x, int y);
    void on_button(int x, int y, bool down);
    void on_leave();

    // Чи миша зараз над вікном. Поки її немає, рамку не показуємо взагалі:
    // поверх гри постійна смужка — це шум, а не зручність.
    bool hovered() const { return hovered_; }

    // Чи рамка зараз хоче мишу (курсор над кнопкою, тягнуть повзунок). Поки
    // хоче — вікно не можна робити клік-крізь, інакше натискання провалиться.
    bool wants_mouse() const;

    // Куди потрапило натискання: у смужку (тягнути вікно), у куточок
    // (розтягувати) чи в кнопку. Вікно питає це перед тим, як почати драг.
    enum class Hit { None, Strip, Grip, Control };
    Hit hit(int x, int y, int w, int h) const;

    // Підкладка й рамка — ДО чату.
    void draw_backdrop(BLContext* ctx, int w, int h, const Look& look) const;
    // Елементи керування — ПІСЛЯ чату. look міняється на місці.
    ChromeEvents draw_controls(BLContext* ctx, int w, int h, Look* look);

private:
    struct Rect {
        float x = 0, y = 0, w = 0, h = 0;
        bool has(float px, float py) const {
            return px >= x && px < x + w && py >= y && py < y + h;
        }
    };

    // Розкладка кнопок залежить лише від ширини вікна, тож рахуємо її на місці
    // й однаково для малювання та для влучань — інакше вони розійшлися б.
    struct Layout {
        Rect strip, lock, zoom_out, zoom_in, opacity, gear, grip;
    };
    Layout layout(int w, int h) const;

    void button(BLContext* ctx, const Rect& r, const char* label, bool active) const;
    void text(BLContext* ctx, const char* s, float x, float baseline,
              const BLRgba32& color, float px) const;

    FontStore* fonts_ = nullptr;
    bool hovered_ = false;
    int mx_ = -1, my_ = -1;
    bool down_ = false;
    // Що саме тягнуть зараз: повзунок прозорості (інших тягнути нема чого).
    bool drag_opacity_ = false;
    // Натискання рахуємо по ВІДПУСКАННЮ над тією ж кнопкою — так само, як
    // поводяться кнопки скрізь: з'їхав курсор до відпускання, значить передумав.
    Rect pressed_{};
    bool have_pressed_ = false;
};

}  // namespace hominka
