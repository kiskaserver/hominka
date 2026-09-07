"""Пастки для перевірки CSS: чи ловить редактор те, що має, і чи мовчить там,
де все гаразд.

Розбір CSS у csslint.cpp — свій і ручний, без готового парсера. Саме такі речі
ламаються тихо: помилкове попередження дратує не менше, ніж пропущена помилка,
а побачити його можна лише відкривши редактор і придивившись. Тому пастки
ганяються окремо й проти справжнього бінаря.

    python csslint_smoke.py [шлях до hominka-render-x64.exe]
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = os.path.join(os.path.dirname(HERE), "dist", "hominka-render-x64.exe")

# (назва, css, очікувані помилки, очікувані попередження)
#
# Очікуване — це СПИСОК ПІДРЯДКІВ: перевіряємо суть повідомлення, а не його
# формулювання. Інакше тест ламався б від кожної правки тексту, і його
# перестали б запускати.
CASES = [
    ("здорова тема",
     ".m { background: rgba(0,0,0,.5); border-radius: 10px; }\n"
     ".n { color: #ffd166 !important; }\n",
     [], []),

    ("незакрита дужка",
     ".m {\n  color: #fff;\n",
     ["блок { не закрито"], []),

    ("зайва дужка",
     ".m { color: #fff; }\n}\n",
     ["зайва }"], []),

    ("оголошення без двокрапки",
     ".m {\n  color #fff;\n}\n",
     ["без двокрапки"], []),

    ("властивість без значення",
     ".m {\n  color: ;\n}\n",
     ["без значення"], []),

    ("незакритий коментар",
     ".m { color: #fff; }\n/* ой\n",
     ["коментар"], []),

    # Пастка: значення в лапках містить двокрапку. Наївний розбір бачить тут
    # «властивість без значення» — а насправді все гаразд.
    ("двокрапка в лапках",
     ".n::after {\n  content: ':';\n}\n",
     [], []),

    # Пастка: селектор із псевдокласом теж має двокрапку, але він ПОЗА блоком —
    # і властивістю не є.
    ("псевдоклас у селекторі",
     ".m:hover {\n  background: #222;\n}\n",
     [], []),

    # Пастка: закоментоване не має давати попереджень.
    ("непідтримуване в коментарі",
     "/* .m { animation: fade 1s; } */\n.m { color: #fff; }\n",
     [], []),

    ("непідтримуване насправді",
     ".m {\n  backdrop-filter: blur(6px);\n  letter-spacing: 2px;\n}\n",
     [], ["backdrop-filter", "letter-spacing"]),

    # Виняток: «animation: none» програма розуміє — це вимкнення появи рядка.
    ("animation: none — не попередження",
     ".m { animation: none; }\n",
     [], []),

    # Виняток: drop-shadow ми перекладаємо в тінь тексту.
    ("filter: drop-shadow — не попередження",
     ".em { filter: drop-shadow(0 2px 2px rgba(0,0,0,.3)); }\n",
     [], []),

    ("інший filter — попередження",
     ".em { filter: blur(2px); }\n",
     [], ["filter"]),

    ("@keyframes і @media",
     "@keyframes slide { from { opacity: 0; } to { opacity: 1; } }\n"
     "@media (min-width: 100px) { .m { color: #fff; } }\n",
     [], ["@keyframes", "@media"]),

    ("складене ім'я властивості",
     ".m { grid-template-columns: 1fr 1fr; }\n",
     [], ["grid-template-columns"]),

    ("display: grid",
     "#list { display: grid; }\n",
     [], ["display: grid"]),
]


def run(exe, css):
    with tempfile.NamedTemporaryFile("w", suffix=".css", encoding="utf-8",
                                     delete=False) as f:
        f.write(css)
        path = f.name
    try:
        out = subprocess.run([exe, "--csslint", path], capture_output=True,
                             timeout=30).stdout.decode("utf-8", "replace")
    finally:
        os.unlink(path)
    errors = [ln[6:] for ln in out.splitlines() if ln.startswith("error ")]
    warns = [ln[5:] for ln in out.splitlines() if ln.startswith("warn ")]
    return errors, warns


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    if not os.path.isfile(exe):
        print("не знайшов %s" % exe)
        return 1

    bad = 0
    for name, css, want_err, want_warn in CASES:
        errors, warns = run(exe, css)
        problems = []
        for want in want_err:
            if not any(want in e for e in errors):
                problems.append("немає помилки про «%s»" % want)
        for want in want_warn:
            if not any(want in w for w in warns):
                problems.append("немає попередження про «%s»" % want)
        if not want_err and errors:
            problems.append("зайві помилки: %s" % errors)
        if not want_warn and warns:
            problems.append("зайві попередження: %s" % warns)
        # Кількість теж важить: одне зайве попередження на кожен рядок теми
        # робить список нечитабельним.
        if want_warn and len(warns) != len(want_warn):
            problems.append("попереджень %d, чекали %d: %s"
                            % (len(warns), len(want_warn), warns))

        if problems:
            bad += 1
            print("НЕ ТАК  %s" % name)
            for p in problems:
                print("        %s" % p)
        else:
            print("гаразд  %s" % name)

    print("")
    print("пасток: %d, невдалих: %d" % (len(CASES), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
