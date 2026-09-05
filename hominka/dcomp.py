"""Керування нативним DirectComposition-оверлеєм (native/hominka-dcomp-x64.exe).

Навіщо. У безрамковому повноекранному (Independent Flip) звичайне вікно чату
зникає — DWM сканує кадр гри повз композицію робочого столу. Окремий процес
малює чат через DirectComposition (йому DWM дає окрему апаратну overlay-площину),
тож чат лишається видимим поверх гри й прихованим від OBS. У ГРУ НІЧОГО НЕ
ВКЛАДАЄТЬСЯ — безпечно для античитів (Hunt: Showdown / EAC).

Кадр чату процес бере зі спільної памʼяті — того самого продюсера, що й для
інжект-оверлея (gameoverlay.py пише BGRA). Тут лише життєвий цикл: запустити
процес, ставити його вікно туди ж, де вікно чату, і зупинити. Розмір вікна веде
сам процес (за розміром кадру), ми задаємо лише позицію.
"""

import ctypes
import os
import subprocess
import sys

from .inject import native_dir

IS_WINDOWS = sys.platform == "win32"
_WND_CLASS = "HominkaDCompOverlay"   # клас вікна, який реєструє exe (шукаємо ним)
_EXE = "hominka-dcomp-x64.exe"


class DCompOverlay:
    def __init__(self):
        self._proc = None
        self._hwnd = 0

    def available(self) -> bool:
        return IS_WINDOWS and os.path.isfile(os.path.join(native_dir(), _EXE))

    def alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def start(self) -> bool:
        if not self.available():
            return False
        if self.alive():
            return True
        exe = os.path.join(native_dir(), _EXE)
        try:
            # CREATE_NO_WINDOW — без консолі; процес сам робить своє topmost-вікно.
            self._proc = subprocess.Popen([exe], creationflags=0x08000000)
        except OSError:
            self._proc = None
            return False
        self._hwnd = 0
        return True

    def stop(self):
        if self._proc is not None:
            try:
                self._proc.terminate()
            except Exception:
                pass
            self._proc = None
        self._hwnd = 0

    def _find(self) -> int:
        if self._hwnd:
            return self._hwnd
        if not IS_WINDOWS:
            return 0
        h = ctypes.windll.user32.FindWindowW(_WND_CLASS, None)
        self._hwnd = int(h) if h else 0
        return self._hwnd

    def place(self, x: int, y: int):
        """Ставить вікно оверлея в (x,y) на екрані, зверху. Розмір НЕ чіпаємо —
        його веде сам процес за розміром кадру чату."""
        if not self.alive():
            return
        hwnd = self._find()
        if not hwnd:
            return
        HWND_TOPMOST = -1
        SWP_NOSIZE = 0x0001
        SWP_NOACTIVATE = 0x0010
        try:
            ctypes.windll.user32.SetWindowPos(hwnd, HWND_TOPMOST, int(x), int(y),
                                              0, 0, SWP_NOSIZE | SWP_NOACTIVATE)
        except Exception:
            pass
