// Панель налаштувань — сама панель, без вікна під нею.
//
// Тут немає жодного виклику Windows чи X11: тільки ImGui. Так само, як
// розкладка чату не знає, чим її малюють, налаштування не знають, у якому
// вікні вони лежать, — і той самий файл згодиться, коли панель з'явиться на
// Linux.
//
// Влаштована вона як розділи ліворуч і одна сторінка праворуч, а не суцільним
// стовпцем. Причина проста: у стовпці все видно одночасно, тому не видно
// нічого — те, по що прийшли, лежить десь усередині, і до нього треба
// докрутити. Розділів п'ять, кожен уміщається без прокрутки, а внизу завжди
// видно головне: які джерела зараз читаються.
//
// Що показуємо, а що ні: у панелі лише те, що програма справді вміє ЗАРАЗ.
// Розділ із непрацюючою кнопкою гірший за його відсутність — він обіцяє те,
// чого немає.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"

namespace hominka {

// Поля вводу тримають свій текст, доки його правлять: писати кожну літеру
// одразу в налаштування означало б перепідключатися до каналу «t», «tw», «twi».
struct SettingsState {
    // YouTube окремо просторіший за решту: сюди вставляють не лише «@нік», а
    // й посилання на канал, а нік із кирилицею в закодованому вигляді — це
    // шість байтів на літеру. «https://www.youtube.com/@ДаниилКириченко-ч8к/
    // streams» після кодування — уже під двісті.
    char youtube[512] = {0};
    char twitch[128] = {0};
    char kick[128] = {0};
    char site[256] = {0};
    bool synced = false;      // текст уже взято з налаштувань

    int page = 0;             // який розділ відкрито
    bool show_injector = false;   // розгорнуто «для досвідчених»

    void sync(const Config& cfg);
};

// Одне джерело чату очима панелі. Стан — кольоровою крапкою, причина — у
// підказці: речення «Twitch: під'єднуюся…» посеред інших речень читає лише
// той, хто його шукає.
struct SourceView {
    std::string name;
    bool configured = false;
    bool connected = false;
    std::string note;
    // Глядачі: «немає числа» і «нуль глядачів» — різні речі, тому окремий
    // прапорець, а не -1.
    bool viewers_known = false;
    std::string viewers;       // уже з пробілами між тисячами
};

// Стан оновлення. Сам оновлювач сюди не заглядає: він Windows-only, а панель
// має лишатися такою ж портативною, як і решта малювання.
struct UpdateView {
    bool supported = false;

    // Стан і опис — окремо, і це не дрібниця. Раніше вони йшли одним рядком, і
    // опис випуску (кілька абзаців зі списками) читався як продовження фрази
    // «У вас найсвіжіша версія». Тепер стан — короткий рядок, опис — те, що
    // написав автор випуску, з його ж поділом на рядки.
    std::string status;
    std::string title;         // «Бета 3.0.0 · велике оновлення»
    std::string notes;
    std::string warning;       // те, що треба прочитати ПЕРЕД встановленням
    std::string size;          // «3 МБ» — щоб кнопка не обіцяла невідомого

    int percent = -1;          // -1 — не качаємо
    bool can_check = false;
    bool can_download = false;
    bool can_install = false;
    bool available = false;    // є що ставити — рейка це показує крапкою
    bool mandatory = false;
};

// Чат поверх гри. Вікна й інжектор живуть у platform/gamewin.h.
struct GameView {
    bool supported = false;
    bool injector = false;
    std::vector<std::string> windows;
    int picked = 0;
    std::string status;
    bool fso_off = false;
    bool restorable = false;
    bool injected = false;
};

// Тема, яку пропонує сайт: людина клацнула «Встановити» на hominka.app, і
// Windows передала нам посилання hominka://theme/<ім'я>.
//
// Ставимо не мовчки. Тема — це весь вигляд чату, і підмінити його без питання
// означало б зробити з посилання зброю: досить підсунути його стрімеру в чат.
// Тому показуємо картку з назвою й двома кнопками, а попередній CSS тримаємо
// напохваті, доки програма працює.
struct ThemeOffer {
    bool pending = false;      // є що показати
    bool loading = false;      // ще качаємо з hominka.app
    std::string id;
    std::string name;          // «Аніме» — як її звуть на сайті
    std::string error;         // не викачалася: покажемо причину, а не тишу
    bool installed = false;    // щойно поставили
    bool can_undo = false;     // є що повертати
};

// Що людина зробила.
struct SettingsEvents {
    bool changed = false;          // налаштування змінилися — зберегти
    bool sources_changed = false;  // канали інші — перепідключитися
    bool look_changed = false;     // вигляд вікна чату треба оновити
    bool motion_changed = false;   // анімовані емоути тепер поводяться інакше
    bool close = false;
    bool css_editor = false;
    bool title_active = false;     // тягнуть за заголовок

    bool check_update = false;
    bool start_download = false;
    bool do_install = false;

    bool install_theme = false;    // «Встановити» в картці теми
    bool cancel_theme = false;     // «Не треба»
    bool undo_theme = false;       // «Повернути попередній CSS»

    bool refresh_games = false;
    bool make_borderless = false;
    bool restore_window = false;
    bool fix_fso = false;
    bool inject = false;
    bool stop_inject = false;
    int  pick_game = -1;
};

// Готує вигляд ImGui під нашу панель. Кличеться один раз на контекст.
void settings_style();

// facts — рядок для «Про програму»: скільки памʼяті займаємо, який розмір
// вікна, скільки джерел читаємо. Складається зовні, бо памʼять — це вже
// система, а панель має лишатися портативною.
SettingsEvents draw_settings(SettingsState* st, Config* cfg,
                             const std::vector<SourceView>& sources,
                             const UpdateView& upd, const GameView& game,
                             const ThemeOffer& offer,
                             const std::string& facts, int w, int h);

}  // namespace hominka
