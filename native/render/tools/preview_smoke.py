"""Перевірка перегляду в редакторі CSS (--preview).

Що саме перевіряємо: редактор більше не тримає браузера, а показує кадр із
того самого рушія, що малює справжній чат. Тож помилитися тут можна двома
способами — і обидва тихі:

  1. кадри не йдуть (окремий процес не піднявся, канал не той) — редактор
     покаже порожнечу й нічого не скаже;
  2. кадри йдуть, але постійно, без упину — а весь виграш нативного рендера
     саме в тому, що намальований рядок не перемальовується. Колись так і
     було: Feed::dirty() рахував і рядки, що виїхали за край, тож перегляд
     молотив 60 к/с на нерухомій картинці.

Тому дивимося не «чи є кадр», а скільки їх за спокійну секунду.

  .venv\\Scripts\\python.exe native\\render\\preview_smoke.py
"""
import os
import sys
import time

from paths import APP, shot_path
sys.path.insert(0, APP)

from PySide6.QtCore import QTimer  # noqa: E402
from PySide6.QtGui import QImage  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from hominka.cssui.catalog import SAMPLES  # noqa: E402
from hominka.nativerender import NativeRenderer  # noqa: E402

OUT = shot_path("preview.png")

# CSS із рівно тих чотирьох речей, які ми доробили після першого заходу. Якщо
# котрась відвалиться, це видно на знімку, а не лише в тесті.
CSS = """
.m { opacity: .9; box-shadow: 0 2px 6px rgba(0,0,0,.6); }
.b { transform: rotate(-6deg); }
.t { color: #cfe; }
"""


def main():
    app = QApplication([])
    r = NativeRenderer(None, preview=True)
    if not r.available():
        print("нативного рендера немає — нічого перевіряти")
        return 2
    if not r.start():
        print("ПОМИЛКА: процес перегляду не піднявся")
        return 1

    frames = []
    r.frame.connect(lambda w, h, d: frames.append((w, h, d)))
    r.set_config(zoom=1.0, width=430, height=420)
    r.set_layout(["ico", "badges", "reply", "name", "money", "text"])
    r.set_css(CSS)
    for s in SAMPLES[:8]:
        r.message(s)

    # Перша фаза: рядки приїжджають і програється поява — кадри мають іти.
    deadline = time.time() + 4.0
    while time.time() < deadline:
        app.processEvents()
        time.sleep(0.01)
    got = len(frames)
    print("кадрів, доки рядки з'являлися: %d" % got)
    if got == 0:
        print("ПОМИЛКА: жодного кадру — перегляд порожній")
        r.stop()
        return 1

    # Друга фаза: нічого не змінюємо. Картинка нерухома, отже кадрів має бути
    # обмаль. Поріг свідомо м'який: важливо відрізнити «майже нічого» від
    # «60 к/с», а не вилизати число.
    frames.clear()
    deadline = time.time() + 2.0
    while time.time() < deadline:
        app.processEvents()
        time.sleep(0.01)
    idle = len(frames)
    print("кадрів за 2 с спокою: %d" % idle)

    # Останній кадр — на диск, щоб було на що подивитися очима.
    if frames or got:
        w, h, d = (frames or [(0, 0, b"")])[-1] if frames else (0, 0, b"")
        if w <= 0:
            # У спокої кадрів могло не бути зовсім — це найкращий випадок.
            r.set_css(CSS + "\n.n { letter-spacing: .3px; }")
            deadline = time.time() + 1.5
            while time.time() < deadline and not frames:
                app.processEvents()
                time.sleep(0.01)
            if frames:
                w, h, d = frames[-1]
        if w > 0:
            img = QImage(d, w, h, w * 4, QImage.Format_ARGB32_Premultiplied)
            img.copy().save(OUT)
            print("знімок перегляду: %s (%dx%d)" % (OUT, w, h))

    r.stop()
    QTimer.singleShot(0, app.quit)

    if idle > 30:
        print("ПОМИЛКА: %d кадрів на нерухомій картинці — рендер молотить дарма" % idle)
        return 1
    print("гаразд: кадри йдуть, у спокої рендер мовчить")
    return 0


if __name__ == "__main__":
    sys.exit(main())
