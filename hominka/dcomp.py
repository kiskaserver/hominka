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
from ctypes import wintypes

from .inject import native_dir

IS_WINDOWS = sys.platform == "win32"
_WND_CLASS = "HominkaDCompOverlay"   # клас вікна, який реєструє exe (шукаємо ним)
_EXE = "hominka-dcomp-x64.exe"

if IS_WINDOWS:
    # Без argtypes/restype ctypes бере HWND за c_int і ОБРІЗАЄ 64-бітний
    # вказівник — саме через це SetWindowPos рухав не те вікно, і оверлей
    # застрягав у лівому куті. Оголошуємо типи явно.
    _u32 = ctypes.windll.user32
    _u32.FindWindowW.restype = wintypes.HWND
    _u32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    _u32.SetWindowPos.restype = wintypes.BOOL
    _u32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
    _u32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]


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
            # Передаємо свій PID: процес стежить за нами й вийде, якщо Hominka
            # зникне (навіть при аварійному завершенні) — без сиріт на екрані.
            self._proc = subprocess.Popen([exe, str(os.getpid())], creationflags=0x08000000)
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

    def _find(self):
        if self._hwnd:
            return self._hwnd
        if not IS_WINDOWS:
            return None
        h = _u32.FindWindowW(_WND_CLASS, None)
        self._hwnd = h if h else None
        return self._hwnd

    def place(self, x: int, y: int):
        """Ставить вікно оверлея в (x,y) на екрані, зверху й видимим. Розмір НЕ
        чіпаємо — його веде сам процес за розміром кадру чату."""
        if not self.alive():
            return
        hwnd = self._find()
        if not hwnd:
            return
        HWND_TOPMOST = ctypes.c_void_p(-1)
        SW_SHOWNA = 8
        SWP_NOSIZE = 0x0001
        SWP_NOACTIVATE = 0x0010
        SWP_SHOWWINDOW = 0x0040
        try:
            _u32.ShowWindow(hwnd, SW_SHOWNA)
            _u32.SetWindowPos(hwnd, HWND_TOPMOST, int(x), int(y), 0, 0,
                              SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)
        except Exception:
            pass

    def hide(self):
        """Ховає вікно оверлея (коли попереду не гра) — щоб на робочому столі не
        дублювати вікно чату."""
        if not IS_WINDOWS:
            return
        hwnd = self._find()
        if hwnd:
            SW_HIDE = 0
            try:
                _u32.ShowWindow(hwnd, SW_HIDE)
            except Exception:
                pass
