// Панель налаштувань — сама панель, без вікна під нею.
//
// Тут немає жодного виклику Windows чи X11: тільки ImGui. Так само, як
// розкладка чату не знає, чим її малюють, налаштування не знають, у якому вікні
// вони лежать, — і той самий файл згодиться, коли панель з'явиться на Linux.
//
// Що показуємо, а що ні: у панелі лише те, що програма справді вміє ЗАРАЗ.
// Порожній розділ «Оновлення» з непрацюючою кнопкою гірший за його відсутність
// — він обіцяє те, чого немає.
#pragma once

#include <string>
#include <vector>

#include "config.h"

namespace hominka {

// Поля вводу тримають свій текст, доки його правлять: писати кожну літеру
// одразу в налаштування означало б перепідключатися до каналу «t», «tw», «twi».
struct SettingsState {
    char youtube[256] = {0};
    char twitch[128] = {0};
    char kick[128] = {0};
    bool synced = false;      // текст уже взято з налаштувань

    void sync(const Config& cfg);
};

// Стан оновлення — очима панелі. Сам оновлювач сюди не заглядає: він
// Windows-only, а панель має лишатися такою ж портативною, як і решта
// малювання. Тому платформна частина складає цю табличку, а панель її показує.
struct UpdateView {
    bool supported = false;    // на цій системі оновлення взагалі є
    std::string status;        // рядок для людини
    int percent = -1;          // -1 — не качаємо
    bool can_check = false;
    bool can_download = false;
    bool can_install = false;
    bool mandatory = false;    // критичне: наполягаємо
};

// Чат поверх гри — очима панелі. Вікна й інжектор живуть у gamewin.h, який
// суто віконний і Windows-only; сюди приходить уже готовий список.
struct GameView {
    bool supported = false;         // на цій системі це взагалі є
    bool injector = false;          // поруч лежать injector.exe і overlay.dll
    std::vector<std::string> windows;   // «назва — exe» по одному рядку
    int picked = 0;
    std::string status;
    bool fso_off = false;           // у вибраної гри вже знято оптимізацію
    bool restorable = false;        // є вікно, якому можна повернути рамку
    bool injected = false;          // чат у грі зараз малюється
};

// Що людина зробила.
struct SettingsEvents {
    bool changed = false;          // налаштування змінилися — зберегти
    bool sources_changed = false;  // канали інші — перепідключитися
    bool look_changed = false;     // вигляд вікна чату треба оновити
    bool close = false;
    bool css_editor = false;       // відкрити редактор теми
    bool check_update = false;
    bool start_download = false;
    bool do_install = false;
    bool refresh_games = false;
    bool make_borderless = false;
    bool restore_window = false;
    bool toggle_fso = false;
    bool inject = false;
    bool stop_inject = false;
    int  pick_game = -1;            // обрали інший рядок у списку
    bool title_active = false;     // тягнуть за заголовок (вікно рухає платформа)
    int content_height = 0;        // скільки насправді треба висоти
};

// Готує вигляд ImGui під нашу панель. Кличеться один раз на контекст.
void settings_style();

// Малює панель шириною w. Значення міняє прямо в cfg.
SettingsEvents draw_settings(SettingsState* st, Config* cfg, const std::string& status,
                             const UpdateView& upd, const GameView& game, int w, int h);

}  // namespace hominka
