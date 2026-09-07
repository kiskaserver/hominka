"""Рамка вікна під Linux: замок, кегль, шестерня, підкладка.

Чому окремою перевіркою. Рамка — єдине місце, де людина торкається оверлея
мишею, і зламатися вона може тихо трьома способами: не з'явитися при наведенні,
з'явитися але не ловити натискання, або ловити їх не тими кнопками (розкладка
для малювання й розкладка для влучань — це два різні шматки коду, і вони
розходяться першими).

Тому мишу підробляємо через XTest (xdotool) і дивимося на ПОДІЇ, які прийшли
клієнту, а не на те, що намалювалося: подія «lock» доводить, що натиснуто саме
замок, а не сусідню кнопку.

    Xvfb :99 -screen 0 1280x720x24 &
    DISPLAY=:99 python3 /src/render/chrome_linux_smoke.py
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pyclient_smoke import install_qt_stub  # noqa: E402

WIN_X, WIN_Y, WIN_W, WIN_H = 40, 40, 430, 300

# Розкладка з chrome_bl.cpp: смужка 22 px, відступ 3, кнопки по 28/26/26,
# шестерня притиснута до правого краю. Числа тут ЗАДУБЛЬОВАНІ навмисно — якщо
# розкладка поїде, перевірка це спіймає, а не підлаштується.
BTN_Y = WIN_Y + 13
LOCK = (WIN_X + 19, BTN_Y)
ZOOM_OUT = (WIN_X + 49, BTN_Y)
ZOOM_IN = (WIN_X + 78, BTN_Y)
GEAR = (WIN_X + 2 + WIN_W - 4 - 3 - 12, BTN_Y)
EMPTY = (WIN_X + 200, WIN_Y + 200)          # нижче смужки, поза кнопками


def x(*a):
    subprocess.run(["xdotool"] + [str(v) for v in a], check=False)


def click(pos):
    x("mousemove", pos[0], pos[1])
    time.sleep(0.15)
    x("mousedown", "1")
    time.sleep(0.10)
    x("mouseup", "1")
    time.sleep(0.35)


def main():
    if not os.environ.get("DISPLAY"):
        print("немає DISPLAY — запускай під Xvfb")
        return 2

    install_qt_stub()
    sys.path.insert(0, "/src")
    import hominka.inject as inject
    inject.native_dir = lambda: "/out"
    import hominka.nativerender as nr
    nr.native_dir = lambda: "/out"
    from hominka.nativerender import NativeRenderer

    r = NativeRenderer(None)
    if not r.available() or not r.start():
        print("ПОМИЛКА: рендер не запустився")
        return 1

    got = []
    r.event.connect(lambda ev: got.append(ev))
    r.set_layout(["ico", "badges", "name", "text"])
    r.set_config(zoom=1.0, width=WIN_W, height=WIN_H, x=WIN_X, y=WIN_Y)
    r.set_css("")
    for i in range(3):
        r.message({"platform": "twitch", "name": "Рамка", "nick": "chrome",
                   "color": "#a970ff", "text": "рядок %d" % i, "badges": []})
    time.sleep(2.0)

    # 1. Наведення. Поки миші немає, рамки не має бути взагалі — постійна
    #    смужка поверх гри це шум. Тому спершу відводимо курсор.
    x("mousemove", 900, 600)
    time.sleep(0.5)
    shot_away = shot(r, "/tmp/chrome_away.png")
    x("mousemove", EMPTY[0], EMPTY[1])
    time.sleep(0.5)
    shot_over = shot(r, "/tmp/chrome_over.png")
    # Рахуємо не «скільки непрозорого» — підкладка накриває верх вікна й без
    # смужки, — а СКІЛЬКИ ЗМІНИЛОСЯ між двома знімками у верхніх 24 рядках.
    # Це прямо відповідає на питання «чи з'явилося щось».
    appeared = top_diff(shot_away, shot_over)
    print("пікселів, що змінилися у смужці після наведення: %d" % appeared)

    # 2. Кнопки.
    got.clear()
    click(GEAR)
    settings = [e for e in got if e.get("t") == "settings"]
    print("шестерня -> подій «settings»: %d" % len(settings))

    got.clear()
    click(LOCK)
    locks = [e for e in got if e.get("t") == "lock"]
    print("замок -> подій «lock»: %d%s" % (len(locks),
          (", on=%s" % locks[-1].get("on")) if locks else ""))
    # Знімаємо назад, інакше вікно лишиться клік-крізь і решта не натиснеться.
    if locks and locks[-1].get("on"):
        r.set_config(locked=False)
        time.sleep(0.4)

    got.clear()
    click(ZOOM_IN)
    looks = [e for e in got if e.get("t") == "look"]
    print("A+ -> подій «look»: %d%s" % (len(looks),
          (", zoom=%.2f" % looks[-1].get("zoom", 0)) if looks else ""))

    r.stop()

    bad = 0
    if appeared < 800:
        print("ПОМИЛКА: смужка не з'являється при наведенні")
        bad += 1
    if not settings:
        print("ПОМИЛКА: шестерня не спрацювала")
        bad += 1
    if not locks:
        print("ПОМИЛКА: замок не спрацював")
        bad += 1
    if not looks or looks[-1].get("zoom", 0) <= 1.0:
        print("ПОМИЛКА: A+ не змінив кегль")
        bad += 1
    if bad:
        return 1
    print("\nгаразд: рамка з'являється при наведенні, кнопки шлють свої події")
    return 0


def shot(r, path):
    if os.path.exists(path):
        os.unlink(path)
    r.send("shot", path=path)
    for _ in range(60):
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return path
        time.sleep(0.05)
    return None


def top_diff(a, b):
    """Скільки пікселів у верхніх 24 рядках відрізняються між двома знімками."""
    if not a or not b:
        return 0
    from app_linux_smoke import decode_png
    aw, ah, ar = decode_png(a)
    bw, bh, br = decode_png(b)
    if aw != bw:
        return 0
    n = 0
    for y in range(min(24, ah, bh)):
        la, lb = ar[y], br[y]
        for px in range(0, aw * 4, 4):
            if (abs(la[px] - lb[px]) + abs(la[px + 1] - lb[px + 1]) +
                    abs(la[px + 2] - lb[px + 2]) + abs(la[px + 3] - lb[px + 3])) > 16:
                n += 1
    return n


if __name__ == "__main__":
    sys.exit(main())
