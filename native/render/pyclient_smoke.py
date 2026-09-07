"""Перевірка Python-клієнта проти Linux-рендера.

Чим це відрізняється від linux_smoke.py. Той говорить із рендером «сам»:
складає кадри руками й доводить, що канал і вікно працюють. А тут працює
СПРАВЖНІЙ клієнт із hominka/nativerender.py — той самий код, що поїде до
людини. Між ними легко розійтися: у клієнта свій потік-писар, своя черга, свій
спосіб відкривати канал, і будь-яка з цих речей могла лишитися віконною.

Qt тут підмінений маленькою заглушкою. Це свідомо: перевіряємо СВІЙ транспорт,
а не машинерію сигналів PySide6 — вона й так щодня працює на Windows, а тягти
в перевірочний образ 150 МБ заради двох рядків не варто. Заглушка дає рівно те,
чим користується nativerender: QObject і Signal із connect/emit.

Запускається ВСЕРЕДИНІ контейнера:

    Xvfb :99 -screen 0 1280x720x24 &
    DISPLAY=:99 python3 /src/render/pyclient_smoke.py
"""
import os
import sys
import time
import types


def install_qt_stub():
    """Мінімальний PySide6: рівно те, що бере nativerender."""
    class Signal:
        def __init__(self, *types_):
            self._name = None

        def __set_name__(self, owner, name):
            self._name = name

        def __get__(self, obj, owner=None):
            if obj is None:
                return self
            slot = "_sig_" + self._name
            if not hasattr(obj, slot):
                setattr(obj, slot, _Bound())
            return getattr(obj, slot)

    class _Bound:
        def __init__(self):
            self._subs = []

        def connect(self, fn):
            self._subs.append(fn)

        def emit(self, *a):
            for fn in self._subs:
                fn(*a)

    class QObject:
        def __init__(self, parent=None):
            pass

    core = types.ModuleType("PySide6.QtCore")
    core.QObject = QObject
    core.Signal = Signal
    pkg = types.ModuleType("PySide6")
    pkg.QtCore = core
    sys.modules["PySide6"] = pkg
    sys.modules["PySide6.QtCore"] = core


def drag():
    """Тягне вікно мишею: натиснути всередині, посунути, відпустити."""
    import subprocess
    def x(*a):
        subprocess.run(["xdotool"] + list(a), check=False)
    x("mousemove", "100", "200")       # усередині вікна (40,40 розміром 430x300)
    x("mousedown", "1")
    for step in range(1, 6):
        x("mousemove", str(100 + step * 12), str(200 + step * 6))
        time.sleep(0.05)
    x("mouseup", "1")


def main():
    if not os.environ.get("DISPLAY"):
        print("немає DISPLAY — запускай під Xvfb")
        return 2

    install_qt_stub()
    sys.path.insert(0, "/src")
    # native_dir шукає теку за маркером; у контейнері бінар лежить у /out.
    import hominka.inject as inject
    inject.native_dir = lambda: "/out"

    from hominka.nativerender import NativeRenderer, _channel
    import hominka.nativerender as nr
    nr.native_dir = lambda: "/out"

    print("канал, який складе клієнт: %s" % _channel("", False))

    r = NativeRenderer(None)
    if not r.available():
        print("ПОМИЛКА: клієнт не бачить бінаря")
        return 1
    if not r.start():
        print("ПОМИЛКА: клієнт не запустив рендер")
        return 1
    print("рендер запущено клієнтом, живий: %s" % ("так" if r.alive() else "НІ"))

    got = []
    r.event.connect(lambda ev: got.append(ev))

    r.set_layout(["ico", "badges", "name", "text"])
    r.set_config(zoom=1.0, width=430, height=300, x=40, y=40)
    r.set_css(".m{background:rgb(0,200,255)}")
    for i in range(3):
        r.message({"platform": "twitch", "name": "Клієнт", "nick": "client",
                   "color": "#a970ff", "text": "повідомлення %d" % i,
                   "badges": ["mod"]})
    time.sleep(2.0)

    shot = "/tmp/pyclient.png"
    if os.path.exists(shot):
        os.unlink(shot)
    r.send("shot", path=shot)
    for _ in range(80):
        if os.path.exists(shot) and os.path.getsize(shot) > 0:
            break
        time.sleep(0.05)

    ok_shot = os.path.exists(shot) and os.path.getsize(shot) > 0
    print("знімок від клієнта: %s (%d байтів)"
          % ("є" if ok_shot else "НЕМАЄ", os.path.getsize(shot) if ok_shot else 0))
    # --- зворотний канал ---------------------------------------------------
    #
    # Єдине, що рендер шле сам, — це «geometry», коли вікно перетягнули. Тож і
    # перевіряємо саме перетягуванням: підробляємо мишу через XTest (xdotool).
    # Так проходить увесь шлях — подія X, наш цикл, сокет, клієнт.
    got.clear()
    drag()
    time.sleep(1.0)
    moves = [e for e in got if e.get("t") == "geometry"]
    print("подій зі зворотного каналу: %d (з них «geometry»: %d)" % (len(got), len(moves)))
    if moves:
        print("останнє: x=%s y=%s" % (moves[-1].get("x"), moves[-1].get("y")))
    asked = bool(moves)

    alive = r.alive()
    r.stop()
    time.sleep(0.3)

    if not alive:
        print("ПОМИЛКА: рендер помер під час роботи")
        return 1
    if not ok_shot:
        print("ПОМИЛКА: клієнт не достукався — кадри не дійшли")
        return 1
    if not asked:
        print("ПОМИЛКА: зворотний канал мовчить — клієнт не почує, що вікно "
              "перетягнули, і геометрія в config.json лишиться старою")
        return 1
    print("\nгаразд: справжній клієнт говорить із Linux-рендером")
    return 0


if __name__ == "__main__":
    sys.exit(main())
