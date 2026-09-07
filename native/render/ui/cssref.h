// Довідник: що можна стилізувати і готові шматки CSS.
//
// Це не «документація десь у файлі», а те, що людина бачить у вікні редактора:
// інакше писати CSS для чужої розмітки — гра у вгадайку.
//
// Таблиці згенеровані з hominka/cssui/catalog.py (див. make_cssref.py), щоб
// довідник у нативному редакторі й у старому Qt-вікні не розійшлися: людина не
// має читати два різні описи того самого класу.
#pragma once

#include <vector>

namespace hominka {

// Готовий шматок: найчастіше людині треба не «дізнатися про клас», а зробити
// одну конкретну річ — збільшити текст, прибрати плашки, підсвітити донати.
struct CssRecipe {
    const char* name;
    const char* hint;
    const char* code;
};

// Селектор, який щось означає в розмітці чату.
struct CssSelector {
    const char* sel;
    const char* title;
    const char* what;
    const char* code;
};

const std::vector<CssRecipe>& css_recipes();
const std::vector<CssSelector>& css_selectors();

}  // namespace hominka
