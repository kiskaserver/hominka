// Перевірка CSS: помилки синтаксису й попередження про непідтримуване.
//
// Дві різні речі, і плутати їх не можна:
//
//   * ПОМИЛКА — правило зникає цілком: незакрита дужка, оголошення без
//     двокрапки. Це видно одразу й ламає все, що нижче.
//   * ПОПЕРЕДЖЕННЯ — правило синтаксично бездоганне, але наш рушій його не
//     вміє (animation, filter, grid…). Він мовчки його викине, верстка не
//     зламається — а людина лишиться з питанням «чому в браузері світилося, а
//     тут ні». Саме на це питання й відповідаємо заздалегідь, ще й підказуючи
//     заміну.
//
// Повного парсера CSS тут немає й не потрібно: шукаємо рівно те, через що
// людина потім не розуміє, що сталося.
//
// Порт hominka/cssui/validator.py і limits.py — разом, бо список
// непідтримуваного потрібен і перевірці, і довіднику. Тримати їх окремо
// означало б, що одного дня вони розійдуться, і в довіднику людина прочитає
// одне, а в підказці — інше.
#pragma once

#include <string>
#include <vector>

namespace hominka {

struct CssProblem {
    int line = 0;
    std::string text;
};

// Чого рушій не вміє: властивість, «що буде», чим замінити.
struct CssNote {
    const char* prop;
    const char* what;
    const char* instead;
};

// Список меншає: те, що трапляється в чужих темах найчастіше, ми доробляємо, а
// не лишаємо тут. Уже вміємо opacity, box-shadow, transform, filter:
// drop-shadow, «.m { animation: none }» і перенос довгих посилань.
const std::vector<CssNote>& css_unsupported();

// Коротка підказка для властивості. nullptr — з нею все гаразд.
const CssNote* css_note(const std::string& prop);

std::vector<CssProblem> validate_css(const std::string& text);
std::vector<CssProblem> lint_css(const std::string& text);

}  // namespace hominka
