"""Перевірка каналу: запускає рендер і шле йому справжній потік повідомлень.

    python native/render/ipc_smoke.py [секунд]

Що перевіряє (те, чого самоперевірка PNG перевірити не може):
  * канал піднімається і Python до нього достукується;
  * повідомлення, видалення, очищення й зміна CSS доходять і діють;
  * картинки їдуть окремим кадром із двійковим вкладенням, і рядок після їх
    приїзду перемальовується з коду емоута на саму картинку;
  * вікно зʼявляється на екрані й ховається від OBS.

Вікно ставимо в лівий верхній кут — щоб було видно очима. Через WDA у запис
екрана воно не потрапить, дивитися треба на самому моніторі.
"""

import ctypes
import os
import sys
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))

from hominka.cssui.catalog import SAMPLES
from hominka.feed.page import DEFAULT_LAYOUT
from hominka.imagefetch import ImageFetcher
from hominka.nativerender import NativeRenderer


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0

    r = NativeRenderer()
    if not r.available():
        print("немає hominka-render-x64.exe у native/dist — спершу збери native")
        return 2
    if not r.start():
        print("рендер не запустився")
        return 3
    print("рендер запущено, pid=%s" % (r._proc.pid,))

    fetch = ImageFetcher(lambda url, data: (print("  картинка приїхала: %s (%d Б)"
                                                  % (url[:60], len(data))),
                                            r.image(url, data)))
    fetch.start()

    r.set_layout(DEFAULT_LAYOUT)
    r.set_css("")
    r.set_config(zoom=1.0, width=430, height=560)
    r.set_enabled(True)

    # Вікно рендера ставимо в кут екрана — воно там і зʼявиться.
    for _ in range(40):
        r.place(60, 60)
        if r._hwnd:
            break
        time.sleep(0.05)
    print("вікно: hwnd=%s" % (r._hwnd or "ще немає",))

    print("шлю %d повідомлень…" % len(SAMPLES))
    for i, m in enumerate(SAMPLES):
        ev = dict(m)
        ev.setdefault("id", "m%d" % i)
        for e in ev.get("emotes") or []:
            fetch.want(e.get("url", ""))
        for b in ev.get("badgeIcons") or []:
            fetch.want(b.get("url", ""))
        r.message(ev)
        time.sleep(0.12)          # той самий темп, що й у чаті (PACE_MS)

    time.sleep(1.5)
    print("прибираю одне повідомлення (delete m3)")
    r.delete("m3")

    time.sleep(1.0)
    print("міняю свій CSS — рядки мають позеленіти")
    r.set_css(".m { background: rgba(0,80,0,.55); border-radius: 10px; padding: 2px 8px; }")

    # Знімок того, що рендер САМЕ ЗАРАЗ показує у вікні. Потрібен, бо вікно
    # приховане від захоплення екрана — звичайний скриншот його не побачить.
    shot = os.path.join(HERE, "live.png")
    if os.path.exists(shot):
        os.remove(shot)
    # Наводимо курсор на смужку вікна: рамка показується лише при наведенні
    # (постійна смужка поверх гри — шум, а не зручність), і без цього на знімку
    # її просто не буде.
    ctypes.windll.user32.SetCursorPos(60 + 120, 60 + 10)
    time.sleep(0.4)
    r.send("shot", path=shot.replace("\\", "/"))
    for _ in range(50):
        if os.path.exists(shot):
            print("знімок живого вікна: %s" % shot)
            break
        time.sleep(0.1)
    else:
        print(r"знімок не зʼявився — дивись у %TEMP%\hominka-overlay.log")

    print("дивись на екран %.0f с (вікно в куті, від OBS сховане)" % seconds)
    time.sleep(seconds)

    print("зупиняю")
    fetch.stop()
    r.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
