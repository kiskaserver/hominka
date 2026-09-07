"""Перемальовує значок «OG» для Kick літерами-контурами.

    python hominka/make_badge_og.py

Навіщо. Kick не віддає картинок значків у чаті, тож стандартні ми малюємо самі
(hominka/badges.py, server/internal/badges/kick.go). Усі, крім «OG», — це
геометричні фігури, а «OG» був єдиним, зробленим через <text>. У браузері він
малювався правильно, а в нативному рендері — ні: там SVG растеризує nanosvg, а
шрифтового рушія в ньому немає, і напис просто зникав, лишаючи порожню плитку.

Замість вигадувати літери руками беремо СПРАВЖНІ обриси Segoe UI (той самий
шрифт, яким значок і малювався в браузері) і зберігаємо їх контуром. Виходить
той самий значок, але його малює будь-хто.

Скрипт друкує готовий data:-рядок для обох місць — і для Python, і для Go.
"""

import base64
import sys

from PySide6.QtGui import QFont, QPainterPath
from PySide6.QtWidgets import QApplication

TEXT = "OG"
BG = "#ff7a00"          # помаранчевий, як і був
FG = "#fff"
BOX = 24.0              # плитка 24×24, як в усіх інших значках
PAD = 3.0               # поля, щоб літери не тиснулися до країв


def path_to_svg(path: QPainterPath) -> str:
    """QPainterPath → рядок «d» для SVG.

    Кубічні криві в Qt приходять трійкою елементів CurveToElement +
    CurveToDataElement×2 — збираємо їх назад в одну команду C.
    """
    out = []
    i = 0
    n = path.elementCount()
    while i < n:
        e = path.elementAt(i)
        if e.type == QPainterPath.MoveToElement:
            out.append("M%.2f %.2f" % (e.x, e.y))
            i += 1
        elif e.type == QPainterPath.LineToElement:
            out.append("L%.2f %.2f" % (e.x, e.y))
            i += 1
        elif e.type == QPainterPath.CurveToElement:
            c1 = path.elementAt(i + 1)
            c2 = path.elementAt(i + 2)
            out.append("C%.2f %.2f %.2f %.2f %.2f %.2f" % (e.x, e.y, c1.x, c1.y, c2.x, c2.y))
            i += 3
        else:
            i += 1
    return "".join(out)


def build() -> str:
    app = QApplication.instance() or QApplication(sys.argv)

    f = QFont("Segoe UI")
    f.setPixelSize(64)              # великий кегль — контур точніший
    f.setWeight(QFont.Weight.Black)

    p = QPainterPath()
    p.addText(0, 0, f, TEXT)
    r = p.boundingRect()
    if r.width() <= 0 or r.height() <= 0:
        raise SystemExit("не вийшло отримати обриси — немає шрифту?")

    # Вписуємо літери в плитку: масштаб по більшій стороні, потім по центру.
    inner = BOX - PAD * 2
    k = min(inner / r.width(), inner / r.height())
    dx = (BOX - r.width() * k) / 2 - r.x() * k
    dy = (BOX - r.height() * k) / 2 - r.y() * k

    from PySide6.QtGui import QTransform
    p2 = QTransform().translate(dx, dy).scale(k, k).map(p)
    d = path_to_svg(p2)

    # fill-rule=evenodd — щоб «O» лишилося кільцем, а не залитим кругом.
    svg = ("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
           "<rect width='24' height='24' rx='6' fill='%s'/>"
           "<path d='%s' fill='%s' fill-rule='evenodd'/></svg>" % (BG, d, FG))
    return "data:image/svg+xml;base64," + base64.b64encode(svg.encode()).decode()


if __name__ == "__main__":
    url = build()
    print("довжина:", len(url))
    print()
    print("для hominka/badges.py:")
    print('    "og": "%s",' % url)
    print()
    print("для server/internal/badges/kick.go:")
    print('\t"og":          "%s",' % url)
