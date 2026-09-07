"""Перевірка того, як рендер розбирає CSS чужих тем.

Навіщо окремо від решти перевірок. Перед видачею в litehtml ми CSS
ПРИСТОСОВУЄМО (adapt_css у cssbits.cpp): «filter: drop-shadow(…)»
переписуємо в «text-shadow», а кожну тінь супроводжуємо власним каналом. Це
текстова обробка, і помилка в ній не лишається локальною: варто загубити
дужку — і відкрита «rgba(» проковтне все, що йде далі, тобто тема втратить не
одне правило, а весь свій хвіст. Побачити таке очима майже неможливо: сторінка
не падає, просто половина стилю тихо не діє.

Саме так і сталося: «filter: drop-shadow(0 1px 2px rgba(0, 0, 0, .5))» шукав
закривальну дужку першою-ліпшою, знаходив ту, що належить rgba(), і решта теми
вимикалася. Тому перевіряємо не «чи намалювалося», а «чи ДІЄ правило, написане
ПІСЛЯ підступного значення».

  .venv\\Scripts\\python.exe native\\render\\css_smoke.py
"""
import io
import json
import os
import subprocess
import sys
import tempfile

from paths import EXE

MSG = {"platform": "twitch", "name": "Tester", "nick": "tester",
       "color": "#a970ff", "text": "проба стилю", "badges": []}

# Маркер: правило, дію якого легко виміряти. Якщо CSS вище зіпсував розбір,
# текст лишиться біля лівого краю замість того, щоб з'їхати праворуч.
MARK = ".m{text-align:right}"


def render(css, tag, tmp):
    j = os.path.join(tmp, "%s.json" % tag)
    p = os.path.join(tmp, "%s.png" % tag)
    io.open(j, "w", encoding="utf-8").write(json.dumps(
        {"width": 430, "zoom": 1.0, "css": css,
         "layout": ["ico", "badges", "reply", "name", "money", "text"],
         "messages": [MSG]}, ensure_ascii=False))
    r = subprocess.run([EXE, "--selftest", j, p], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        return None
    return p


def right_edge(png):
    """Де закінчується фарба. Тло полотна непрозоре, тож міряємо не альфу, а
    відхилення від кольору кутового пікселя."""
    from PIL import Image
    im = Image.open(png).convert("RGBA")
    px = im.load()
    w, h = im.size
    bg = px[1, 1]
    r = -1
    for y in range(h):
        for x in range(w - 1, r, -1):
            c = px[x, y]
            if abs(c[0]-bg[0]) + abs(c[1]-bg[1]) + abs(c[2]-bg[2]) > 24:
                r = x
                break
    return r


# (назва, CSS ПЕРЕД маркером). Кожен рядок — значення, на якому легко
# загубитися текстовому розбору.
CASES = [
    ("нічого перед маркером", ""),
    ("drop-shadow з rgba", ".ico{filter:drop-shadow(0 1px 2px rgba(0, 0, 0, 0.5))}"),
    ("drop-shadow без дужок усередині", ".ico{filter:drop-shadow(0 1px 2px #000)}"),
    ("дві drop-shadow поспіль",
     ".ico{filter:drop-shadow(0 1px 2px rgba(0,0,0,.5))}"
     ".bi{filter:drop-shadow(0 2px 3px rgba(0,0,0,.4))}"),
    ("text-shadow з rgba", ".t{text-shadow:1px 1px 2px rgba(0, 0, 0, 0.8)}"),
    ("крапка з комою в лапках", '.n::after{content:";"}'),
    ("дужка в лапках", '.n::after{content:")"}'),
    ("backdrop-filter", ".m{backdrop-filter:blur(6px)}"),
    ("@keyframes із вкладеними дужками",
     "@keyframes a{0%{opacity:0;transform:translateX(-20px) scale(.95)}"
     "100%{opacity:1;transform:translateX(0) scale(1)}}"),
    ("border-image з градієнтом",
     ".m{border-image:linear-gradient(to bottom,#fbbf24,#f59e0b) 1}"),
]


def main():
    if not os.path.exists(EXE):
        print("нативного рендера немає — нічого перевіряти")
        return 2
    tmp = tempfile.mkdtemp(prefix="hmk-css-")

    base = render(MARK, "base", tmp)
    if base is None:
        print("ПОМИЛКА: рендер не запустився")
        return 1
    want = right_edge(base)
    plain = render("", "plain", tmp)
    idle = right_edge(plain)
    if want - idle < 40:
        print("ПОМИЛКА: маркер не розрізняється (%d проти %d)" % (want, idle))
        return 1
    print("маркер: text-align:right -> правий край %d (без нього %d)" % (want, idle))

    bad = 0
    for name, css in CASES:
        p = render(css + MARK, "c%d" % (abs(hash(name)) % 100000), tmp)
        if p is None:
            print("  ЗБІЙ  %-34s рендер упав" % name)
            bad += 1
            continue
        got = right_edge(p)
        ok = abs(got - want) <= 3
        print("  %-5s %-34s правий край %d" % ("гаразд" if ok else "ЗЛАМАВ", name, got))
        if not ok:
            bad += 1

    if bad:
        print("\n%d випадк(ів) гасять правила, написані нижче — розбір CSS зіпсовано" % bad)
        return 1
    print("\nгаразд: жодне значення не з'їдає правил, що йдуть за ним")
    return 0


if __name__ == "__main__":
    sys.exit(main())
