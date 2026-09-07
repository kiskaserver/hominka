"""Робить cssref.cpp із hominka/cssui/catalog.py.

Навіщо генерувати, а не переписати руками: довідник — це кілька сотень рядків
тексту українською, і переписаний він розійшовся б із оригіналом уже за
місяць. Тут же видно, що обидва вікна показують ОДНЕ Й ТЕ САМЕ.

    python make_cssref.py        # переписує cssref.cpp поруч
"""

import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, ROOT)

from hominka.cssui.catalog import RECIPES, SELECTORS   # noqa: E402


def cstr(s: str) -> str:
    """Рядок Python → літерал C++ (UTF-8 лишається як є, читабельним)."""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            continue
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def main():
    lines = [
        "// Згенеровано make_cssref.py з hominka/cssui/catalog.py — не правити руками.",
        '#include "cssref.h"',
        "",
        "namespace hominka {",
        "",
        "namespace {",
        "",
        "const std::vector<CssRecipe> kRecipes = {",
    ]
    for name, hint, code in RECIPES:
        lines.append("    {%s," % cstr(name))
        lines.append("     %s," % cstr(hint))
        lines.append("     %s}," % cstr(code))
    lines += ["};", "", "const std::vector<CssSelector> kSelectors = {"]
    for sel, title, what, code in SELECTORS:
        lines.append("    {%s," % cstr(sel))
        lines.append("     %s," % cstr(title))
        lines.append("     %s," % cstr(what))
        lines.append("     %s}," % cstr(code))
    lines += [
        "};",
        "",
        "}  // namespace",
        "",
        "const std::vector<CssRecipe>& css_recipes() { return kRecipes; }",
        "const std::vector<CssSelector>& css_selectors() { return kSelectors; }",
        "",
        "}  // namespace hominka",
        "",
    ]
    path = os.path.join(HERE, "cssref.cpp")
    io.open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print("cssref.cpp: %d рецептів, %d селекторів" % (len(RECIPES), len(SELECTORS)))


if __name__ == "__main__":
    main()
