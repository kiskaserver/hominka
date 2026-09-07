"""Предпросмотр редактора CSS під Linux (--preview).

Те саме, що перевіряє preview_smoke.py під Windows, і питання ті самі два, бо
обидві помилки тихі:

  1. кадри не йдуть — редактор покаже порожнечу й нічого не скаже;
  2. кадри йдуть без упину — а весь виграш нативного рендера саме в тому, що
     намальований рядок не перемальовується.

Плюс третє, чого на Windows перевіряти не треба: чи правильно складені пікселі.
Blend2D вирівнює рядок полотна, і він буває довшим за ширину*4; той бік чекає
щільні. Помилка тут дала б косий «зсув» картинки, який на око читається як
розсипаний шрифт, а не як помилка формату. Тому шукаємо у кадрі прикметний
колір у ЗАДАНОМУ місці, а не «десь».

Qt підмінений заглушкою — перевіряємо свій транспорт, а не сигнали PySide6
(докладніше в pyclient_smoke.py).

    Xvfb :99 -screen 0 1280x720x24 &
    DISPLAY=:99 python3 /src/render/preview_linux_smoke.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pyclient_smoke import install_qt_stub  # noqa: E402

W, H = 300, 220
MARK = (0, 200, 255)


def main():
    install_qt_stub()
    sys.path.insert(0, "/src")
    import hominka.inject as inject
    inject.native_dir = lambda: "/out"
    import hominka.nativerender as nr
    nr.native_dir = lambda: "/out"
    from hominka.nativerender import NativeRenderer

    r = NativeRenderer(None, preview=True)
    if not r.available() or not r.start():
        print("ПОМИЛКА: предпросмотр не запустився")
        return 1

    frames = []
    r.frame.connect(lambda w, h, d: frames.append((w, h, bytes(d))))
    r.set_config(zoom=1.0, width=W, height=H)
    r.set_layout(["ico", "badges", "name", "text"])
    r.set_css(".m{background:rgb(0,200,255)}")
    for i in range(3):
        r.message({"platform": "twitch", "name": "Тест", "nick": "test",
                   "color": "#a970ff", "text": "предпросмотр %d" % i,
                   "badges": ["mod"]})

    until = time.time() + 4.0
    while time.time() < until:
        time.sleep(0.02)
    got = len(frames)
    print("кадрів, доки рядки з'являлися: %d" % got)

    # Спокій: нічого не міняємо, кадрів має бути обмаль.
    frames.clear()
    until = time.time() + 2.0
    while time.time() < until:
        time.sleep(0.02)
    idle = len(frames)
    print("кадрів за 2 с спокою: %d" % idle)

    # Формат пікселів. Беремо останній кадр, який справді був.
    r.set_css(".m{background:rgb(0,200,255)}\n.t{color:#fff}")
    until = time.time() + 2.0
    while time.time() < until and not frames:
        time.sleep(0.02)
    ok_px = False
    if frames:
        w, h, d = frames[-1]
        print("кадр: %dx%d, байтів %d (щільно було б %d)" % (w, h, len(d), w * h * 4))
        if len(d) == w * h * 4:
            # Рядки чату притиснуті до низу, тож шукаємо в нижній третині —
            # і саме по рядках, щоб зсув рядка одразу впав в око.
            hits = 0
            for y in range(h * 2 // 3, h):
                off = y * w * 4
                for x in range(0, w, 3):
                    p = off + x * 4
                    b, g, rr = d[p], d[p + 1], d[p + 2]
                    if (abs(rr - MARK[0]) <= 14 and abs(g - MARK[1]) <= 14
                            and abs(b - MARK[2]) <= 14):
                        hits += 1
            print("пікселів нашого тла у нижній третині: %d" % hits)
            ok_px = hits > 300
    r.stop()

    bad = 0
    if got == 0:
        print("ПОМИЛКА: жодного кадру — предпросмотр порожній")
        bad += 1
    if idle > 30:
        print("ПОМИЛКА: %d кадрів на нерухомій картинці" % idle)
        bad += 1
    if not ok_px:
        print("ПОМИЛКА: кадр приїхав, але пікселі складено не так")
        bad += 1
    if bad:
        return 1
    print("\nгаразд: предпросмотр під Linux малює, мовчить у спокої і "
          "віддає щільні пікселі")
    return 0


if __name__ == "__main__":
    sys.exit(main())
