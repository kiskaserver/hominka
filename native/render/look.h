// Вигляд вікна й те, що людина з ним зробила.
//
// Окремим файлом, бо це спільне: рамку малюють два різні файли (ImGui під
// Windows, Blend2D під Linux), а от НАБІР значень і НАБІР подій мусить бути
// один. Інакше «покрутили прозорість» на одній системі означало б не те саме,
// що на іншій, і Python довелося б розбирати два випадки замість одного.
#pragma once

namespace hominka {

// Значення ті самі, що в config.json (opacity, bg_alpha, zoom), і назви
// навмисно ті самі.
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

}  // namespace hominka
