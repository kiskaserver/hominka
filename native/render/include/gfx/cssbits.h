// Розбір CSS — те, що НЕ залежить від того, чим ми малюємо.
//
// Навіщо окремим файлом. Малювання під Windows робить Direct2D, під Linux —
// Blend2D, і це два різні файли. Але тіні, кольори, перетворення й переноси
// слів мусять розбиратися ОДНАКОВО: тема, зроблена на Windows, має виглядати
// так само на Linux. Якби цей код лежав у кожному контейнері свій, розходження
// зʼявилося б не одразу й знайшлося б важко — не «не працює», а «трохи інакше».
//
// Тому все, що зводиться до «прочитати рядок CSS і дістати з нього числа»,
// живе тут, а в контейнерах лишається саме малювання.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "litehtml.h"

namespace hominka {

// Докладний журнал малювання. Умикається --verbose у самоперевірці: коли
// картинка виходить не такою, треба бачити, який шрифт створили і які метрики
// з нього дістали, — на око цього не видно.
extern bool g_draw_trace;
void dtrace(const char* fmt, ...);

// Колір як чотири числа 0..1. Свій тип, а не D2D1_COLOR_F чи BLRgba, саме щоб
// цей файл нічого не знав про рушій малювання; обидва їх приймають один в один.
struct Color {
    float r = 0, g = 0, b = 0, a = 0;
};

inline Color rgba(float r, float g, float b, float a = 1.0f) {
    Color c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}

inline Color to_color(const litehtml::web_color& c) {
    return rgba(c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f, c.alpha / 255.0f);
}

// Тінь тексту: те, що в CSS записано як «text-shadow: 0 2px 3px rgba(0,0,0,.95)».
struct TextShadow {
    bool on = false;
    float dx = 0, dy = 0, blur = 0;
    Color color;
};

// Тінь блока: «box-shadow: 0 4px 12px rgba(0,0,0,.2)».
struct BoxShadow {
    float dx = 0, dy = 0, blur = 0, spread = 0;
    bool inset = false;
    Color color = {0, 0, 0, 1};
};

// Місце, де в рядку стоїть анімований емоут. Координати — у системі картинки
// рядка, тобто рахуються від її лівого верхнього кута.
struct Sprite {
    std::string url;
    float x = 0, y = 0, w = 0, h = 0;
};

// --- розбір ---------------------------------------------------------------

// Один символ UTF-8: код і скільки байтів він зайняв.
unsigned utf8_cp(const std::string& s, size_t i, int* len);

bool parse_color(const std::string& s, Color* out);
bool parse_shadow_value(std::string val, TextShadow* out);
bool parse_box_shadow(const std::string& one, float font_size, BoxShadow* out);

// Кінець оголошення: перша «;» або «}» ЗА межами дужок і лапок.
size_t decl_end(const std::string& s, size_t from);
// Дужка, парна до s[open] == '('.
size_t match_paren(const std::string& s, size_t open);

// Найбільший радіус скруглення й чи всі кути однакові — цим малювання
// вирішує, класти скруглений прямокутник чи звичайний.
float max_radius(const litehtml::border_radiuses& r);
bool same_radius(const litehtml::border_radiuses& r);

// Перетворення («rotate(4deg) scale(1.1) translateX(10px)») у матрицю 3×2,
// записану як [a b c d e f] — той самий порядок, що в Direct2D і в Blend2D.
// Початок відліку — центр прямокутника (типовий transform-origin).
//
// Повертає false, якщо нічого не розібралося: тоді перетворення не ставиться.
bool parse_transform(const std::string& value, float font_size,
                     const litehtml::position& border_box, float m[6]);

// Розбити список тіней («0 2px 4px #000, 0 0 8px red») на окремі значення:
// коми всередині rgba() до уваги не беруться.
std::vector<std::string> split_shadow_list(const std::string& value);

// Проводить text-shadow крізь litehtml (див. довгий коментар у cssbits.cpp).
std::string inject_shadow_channel(const std::string& css);
// «filter: drop-shadow(…)» → «text-shadow: …».
std::string translate_drop_shadow(const std::string& css);
// Стилі, пристосовані до litehtml: обидва перетворення разом.
// zoom — множник кегля («A+»/«A−»). Одиниця означає «нічого не чіпати».
std::string adapt_css(const std::string& css, float zoom = 1.0f);
// Дістає тінь, оголошену для всієї сторінки (правило body).
TextShadow parse_text_shadow(const std::string& css);
// Дістає тінь із токена, який приїхав каналом text-emphasis-style.
bool shadow_from_channel(const std::string& token, TextShadow* out);

// Розбиває текст на слова так, як це має робити чат: додатково дозволяє
// розрив у довгих посиланнях (після «/?&=#-_») і ріже суцільний шматок від
// 28 знаків. litehtml сам рве рядок лише по пробілах, і посилання на пів
// екрана просто виїжджало б за край.
void split_text_words(const char* text,
                      const std::function<void(const char*)>& on_word,
                      const std::function<void(const char*)>& on_space);

}  // namespace hominka
