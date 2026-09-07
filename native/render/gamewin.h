// Чужі вікна: знайти гру, зробити її безрамковою, зняти повноекранну
// оптимізацію — і, якщо людина свідомо цього просить, вкласти в неї чат.
//
// Порт hominka/fullscreen.py та inject.py. Нічого нового тут немає: усе це
// звичайні виклики Win32 і один запис у гілці сумісності HKCU. Саме тому
// перенесення й можливе — на відміну від сторінки чужого сайту, для якої
// потрібен браузер.
//
// Тверда відмова (античити) лишається в самому інжекторі
// (native/common/guard.h), а не тут: перевірку не можна тримати на боці, який
// легше обійти.
#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

namespace hominka {

struct GameWindow {
    void* hwnd = nullptr;
    unsigned pid = 0;
    std::string exe;
    std::string title;
};

// Видимі вікна-кандидати на гру. Замість «вгадай, що зараз попереду» — явний
// вибір зі списку.
std::vector<GameWindow> list_windows();

// Безрамковий режим: знімає з чужого вікна рамку й розтягує на монітор.
// Те саме, що робить Borderless Gaming, і без жодної ін'єкції — стиль вікна
// звичайна властивість, яку можна змінити ззовні. Керує таким вікном уже DWM,
// а отже чат поверх нього видно.
bool make_borderless(void* hwnd);
bool restore_window(void* hwnd);
void* borderless_window();          // те, що ми змінили (nullptr — нічого)

// Повноекранна оптимізація. Прапорець у гілці сумісності HKCU; діє з
// наступного запуску гри й нічого в гру не вкладає.
std::string game_exe_path(void* hwnd);
bool fso_disabled(const std::string& exe_path);
bool set_fso_disabled(const std::string& exe_path, bool disabled);

// --- інжектор -------------------------------------------------------------

// Де лежать нативні частини: поруч із програмою або в dist під час розробки.
std::string native_dir();

// Чи лежать поруч injector-x64.exe і overlay-x64.dll.
bool injector_available();

struct InjectResult {
    bool ok = false;
    bool blocked = false;          // античит: пропонувати ще раз не треба
    unsigned pid = 0;
    std::string message;
};

// Кладе overlay.dll у процес вікна. Повертає людяне пояснення.
InjectResult inject_into(void* hwnd);

// --- Vulkan ---------------------------------------------------------------
//
// Vulkan не можна «вкласти» після старту: гра ініціалізує його одразу при
// запуску, і наш код має бути на місці ЩЕ до того. Тому чат у Vulkan-грі
// показує не вкладена DLL, а НЕЯВНИЙ ШАР — loader сам вантажить його в будь-яку
// Vulkan-гру, якщо шар зареєстровано.
//
// Реєструємо, лише поки ввімкнено чат у грі, і знімаємо, коли вимкнено чи при
// виході: тримати шар зареєстрованим постійно ні до чого — він вантажився б у
// чужі Vulkan-застосунки, яким до нас байдуже.
bool vklayer_register();
void vklayer_unregister();

}  // namespace hominka

#endif  // _WIN32
