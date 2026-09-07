"""Перевірка інжект-шляху: чат усередині чужого процесу.

    python native/render/inject_smoke.py [dx11|dx12|gl|vk]

Це головна перевірка етапу 3. Раніше кадр для overlay.dll робив Python: знімав
картинку з вікна браузера, рахував CRC по всьому кадру й перекладав байти у
спільну память. Тепер його кладе туди сам рендер — і треба переконатися, що
DLL усередині гри бачить рівно те саме.

Що робимо: піднімаємо гру-макет (native/dist/testhost-*.exe), вкладаємо в неї
overlay.dll (для Vulkan — реєструємо імпліцитний шар, бо вкласти після старту
там не можна), вмикаємо рендер, шлемо йому повідомлення й дивимося в журнал, чи
DLL прочитала кадр. Вікно тестового хоста від захоплення НЕ приховане, тож його
видно й звичайним скриншотом.
"""

import os
import subprocess
import sys
import time

from paths import APP as ROOT
sys.path.insert(0, ROOT)

from hominka import inject
from hominka.cssui.catalog import SAMPLES
from hominka.feed.page import DEFAULT_LAYOUT
from hominka.imagefetch import ImageFetcher
from hominka.nativerender import NativeRenderer

HOSTS = {"dx11": "testhost-x64.exe", "dx12": "testhost-dx12-x64.exe",
         "gl": "testhost-gl-x64.exe", "vk": "testhost-vk-x64.exe"}
LOG = os.path.join(os.environ.get("TEMP", "."), "hominka-overlay.log")


def find_window_pid(pid, tries=60):
    """Чекає, поки в процесі зʼявиться вікно (гра малює не з першої мілісекунди)."""
    import ctypes
    from ctypes import wintypes
    u32 = ctypes.windll.user32
    found = []

    CB = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, _l):
        p = wintypes.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(hwnd):
            found.append(hwnd)
            return False
        return True

    for _ in range(tries):
        found.clear()
        u32.EnumWindows(CB(cb), 0)
        if found:
            return found[0]
        time.sleep(0.1)
    return 0


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "dx11"
    host = os.path.join(inject.native_dir(), HOSTS.get(which, HOSTS["dx11"]))
    if not os.path.isfile(host):
        print("немає %s — спершу збери native" % host)
        return 2

    mark = os.path.getsize(LOG) if os.path.exists(LOG) else 0

    # Vulkan не можна «вкласти» після старту: шар має бути на місці ДО запуску.
    # Тому там реєструємо імпліцитний шар і DLL не вкладаємо взагалі.
    if which == "vk":
        from hominka import vklayer
        vklayer.register()
        print("зареєстровано імпліцитний шар Vulkan")

    print("піднімаю гру-макет: %s" % os.path.basename(host))
    game = subprocess.Popen([host])
    hwnd = find_window_pid(game.pid)
    if not hwnd:
        print("вікно гри не зʼявилося")
        game.kill()
        return 3
    print("вікно гри: hwnd=%d pid=%d" % (hwnd, game.pid))

    r = NativeRenderer()
    fetch = ImageFetcher(lambda url, data: r.image(url, data))
    try:
        if not r.start():
            print("рендер не запустився")
            return 3
        fetch.start()
        r.set_layout(DEFAULT_LAYOUT)
        r.set_css("")
        r.set_config(zoom=1.0, width=430, height=560)
        r.set_enabled(True)
        # Вікно рендера ставимо в кут — від нього залежить, ДЕ саме чат
        # зʼявиться в грі (розкладка рахується як частка монітора).
        for _ in range(40):
            r.place(100, 100)
            if r._hwnd:
                break
            time.sleep(0.05)

        if which == "vk":
            target_pid = game.pid
            print("Vulkan: шар уже в грі, вкладати нічого не треба")
        else:
            print("вкладаю overlay.dll…")
            res = inject.inject(hwnd)
            print("  результат: ok=%s pid=%s — %s" % (res.ok, res.pid, res.message))
            if not res.ok:
                return 4
            target_pid = res.pid

        # Ось воно: кажемо рендеру класти кадр у спільну память саме для цього
        # процесу. Доти він туди нічого не пише й з відеокарти нічого не читає.
        r.set_inject(True, pid=target_pid, opacity=235, hide_obs=False)

        for i, m in enumerate(SAMPLES[:12]):
            ev = dict(m)
            ev.setdefault("id", "i%d" % i)
            for e in ev.get("emotes") or []:
                fetch.want(e.get("url", ""))
            r.message(ev)
            time.sleep(0.1)

        print("дивись на вікно гри — у ньому має бути чат (5 с)")
        time.sleep(5)
    finally:
        r.set_inject(False)
        time.sleep(0.3)
        fetch.stop()
        r.stop()
        game.kill()
        if which == "vk":
            from hominka import vklayer
            vklayer.unregister()

    print("\n--- журнал (новий хвіст) ---")
    if os.path.exists(LOG):
        with open(LOG, "r", encoding="utf-8", errors="replace") as f:
            f.seek(mark)
            for ln in f.read().splitlines():
                print("  " + ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
