"""Перевірка звʼязки: справжнє вікно Hominka + нативний рендер.

    python native/render/app_smoke.py

Піднімає програму так само, як звичайний запуск (те саме вікно, та сама
стрічка), примусово вмикає нативний рендер і «чат поверх гри», проганяє крізь
ЗВИЧАЙНУ чергу подій (ChatFeed.push) зразки повідомлень і знімає те, що рендер
показав. Тобто перевіряється саме той шлях, яким ідуть справжні повідомлення
з Twitch/Kick/YouTube, а не окремий тестовий.
"""

import os
import sys

from paths import APP as ROOT, shot_path
sys.path.insert(0, ROOT)

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QApplication

from hominka import config as hconfig
from hominka.cssui.catalog import SAMPLES

# Рішення «піднімати браузер чи ні» приймається ще в конструкторі вікна, за
# налаштуваннями. Підміняємо їх ДО створення Overlay — інакше перевіряли б не
# той шлях.
_real_peek = hconfig.peek
hconfig.peek = lambda: dict(_real_peek(), renderer="native", twitchChannel="test")

from hominka.overlay import Overlay

SHOT = shot_path("app.png")


def _report_cost():
    """Скільки все це коштує: памʼять і зайві процеси.

    Саме заради цих двох чисел етап і затівався, тож міряємо їх, а не віримо
    на слово.
    """
    import ctypes
    import subprocess
    from ctypes import wintypes

    class PMC(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t)]

    def rss(pid=None):
        # restype обовʼязково: без нього ctypes бере псевдо-дескриптор поточного
        # процесу (-1) за int і обрізає його — саме та пастка, через яку в
        # dcomp.py колись рухалося не те вікно.
        k32 = ctypes.windll.kernel32
        k32.GetCurrentProcess.restype = wintypes.HANDLE
        k32.OpenProcess.restype = wintypes.HANDLE
        h = k32.GetCurrentProcess() if pid is None else k32.OpenProcess(0x1000, False, pid)
        psapi = ctypes.windll.psapi
        psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC),
                                               wintypes.DWORD]
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        pmc = PMC()
        pmc.cb = ctypes.sizeof(PMC)
        ok = psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
        return pmc.WorkingSetSize // (1024 * 1024) if ok else 0

    def count(name):
        try:
            out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + name, "/NH"],
                                 capture_output=True, text=True, timeout=10).stdout
            return sum(1 for ln in out.splitlines() if name.lower() in ln.lower())
        except Exception:
            return -1

    print("памʼять Hominka: %d МБ" % rss())
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq hominka-render-x64.exe",
                              "/NH", "/FO", "CSV"], capture_output=True, text=True,
                             timeout=10).stdout
        for ln in out.splitlines():
            parts = [p.strip('"') for p in ln.split('","')]
            if len(parts) >= 5 and parts[0].lower().startswith("hominka-render"):
                print("памʼять рендера: %s" % parts[4])
    except Exception:
        pass
    print("процесів QtWebEngine: %d" % count("QtWebEngineProcess.exe"))
    print("процесів рендера: %d" % count("hominka-render-x64.exe"))


def main():
    os.environ.setdefault("QTWEBENGINE_CHROMIUM_FLAGS", "--enable-features=TranslucentWindows")
    app = QApplication(sys.argv)
    win = Overlay()
    # Вмикаємо нативний рендер примусово: у config.json його могло й не бути,
    # а перевірити треба саме його.
    if not win._native_window:
        print("нативне вікно не ввімкнулося — перевіряти нічого")
        return 2
    if win._native is None:
        print("нативний рендер недоступний (немає бінаря?) — перевіряти нічого")
        return 2
    print("браузер піднято: %s" % ("ТАК" if win.view is not None else "ні"))
    # У нативному режимі Qt-вікно не показуємо: вікном чату стає вікно рендера.
    def step1():
        print("нативне вікно чату: %s" % ("увімкнено" if win.dcomp_on else "НЕ увімкнулося"))
        if not win.dcomp_on:
            print("не ввімкнувся")
            app.quit()
            return
        QTimer.singleShot(600, step2)

    def step2():
        print("шлю %d повідомлень звичайною чергою" % len(SAMPLES))
        for i, m in enumerate(SAMPLES):
            ev = dict(m)
            ev.setdefault("id", "a%d" % i)
            ev.setdefault("kind", ev.get("kind") or "msg")
            win.feed.push(ev)
        QTimer.singleShot(2500, step3)

    def step3():
        if os.path.exists(SHOT):
            os.remove(SHOT)
        win._native.send("shot", path=SHOT.replace("\\", "/"))
        QTimer.singleShot(1500, step4)

    def step4():
      try:
        print("знімок: %s" % (SHOT if os.path.exists(SHOT) else "НЕ зʼявився"))
        print("браузер після роботи: %s" % ("НЕ піднімався" if win.view is None else "Є"))
        _report_cost()

      finally:
        # Що б не сталося у вимірюванні, програму треба закрити — інакше
        # перевірка просто зависає, і замість «не працює» ми отримуємо тишу.
        win.set_dcomp_overlay(False)
        win.close()
        app.quit()

    # Даємо пройти таймеру пошуку ефіру (він спрацьовує через 4 с): саме він
    # раніше й піднімав браузер.
    QTimer.singleShot(6000, step1)
    app.exec()
    return 0


if __name__ == "__main__":
    sys.exit(main())
