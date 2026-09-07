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
    bool title_active = false;     // тягнуть за заголовок (вікно рухає платформа)
    int content_height = 0;        // скільки насправді треба висоти
};

// Готує вигляд ImGui під нашу панель. Кличеться один раз на контекст.
void settings_style();

// Малює панель шириною w. Значення міняє прямо в cfg.
SettingsEvents draw_settings(SettingsState* st, Config* cfg, const std::string& status,
                             const UpdateView& upd, int w, int h);

}  // namespace hominka
