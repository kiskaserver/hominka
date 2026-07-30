"""
Chat Overlay — прозорий оверлей чату поверх гри, НЕВИДИМИЙ для OBS.

Навіщо: у тебе один монітор, і чат, який OBS накладає на трансляцію, ти сам не
бачиш. Ця програма показує твою сторінку чату (/overlay/chat) окремим вікном
поверх усіх ігор, але Windows приховує це вікно від будь-якого захоплення екрана
(OBS Display/Window/Game Capture його НЕ бачить — механізм WDA_EXCLUDEFROMCAPTURE,
Windows 10 2004+ / Windows 11). Тож глядачі бачать чат лише один раз (з OBS), а
ти — окремо поверх гри.

Можливості:
  • перетягування — за верхню панель;
  • зміна розміру — за куточок унизу праворуч (або край вікна);
  • прозорість — повзунок на панелі;
  • «клік-крізь» (Lock) — вікно перестає ловити мишу, кліки йдуть у гру;
    вмикається кнопкою або гарячою клавішею Ctrl+Alt+Space (щоб вимкнути назад,
    коли миша вже проходить крізь, використовуй ту саму гарячу клавішу);
  • запам'ятовує розмір/позицію/прозорість (config.json поруч зі скриптом).

Запуск:  python chat_overlay.py  [URL]
URL за замовчуванням — унизу в CHAT_URL. Можна передати свій першим аргументом.
"""

import ctypes
import json
import os
import sys
from ctypes import wintypes

from PySide6.QtCore import Qt, QUrl, QPoint
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QSizeGrip, QPushButton, QLabel, QSlider,
)
from PySide6.QtWebEngineWidgets import QWebEngineView

# === Налаштування за замовчуванням ==========================================
CHAT_URL = "https://stream.svitix.com/overlay/chat?lang=uk"

CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json")

# === WinAPI константи =======================================================
WDA_EXCLUDEFROMCAPTURE = 0x00000011  # вікно виключене з захоплення екрана
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020       # клік проходить крізь вікно
WS_EX_LAYERED = 0x00080000
WM_HOTKEY = 0x0312
MOD_CONTROL = 0x0002
MOD_ALT = 0x0001
MOD_NOREPEAT = 0x4000
VK_SPACE = 0x20
HOTKEY_ID = 1

user32 = ctypes.windll.user32


def _hwnd(win) -> int:
    return int(win.winId())


def exclude_from_capture(win) -> bool:
    """Приховати вікно від захоплення екрана (OBS його не побачить)."""
    try:
        ok = user32.SetWindowDisplayAffinity(_hwnd(win), WDA_EXCLUDEFROMCAPTURE)
        return bool(ok)
    except Exception:
        return False


def set_click_through(win, enabled: bool):
    """Увімкнути/вимкнути прохід кліків миші крізь вікно (для гри)."""
    hwnd = _hwnd(win)
    ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    if enabled:
        ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED
    else:
        ex &= ~WS_EX_TRANSPARENT
    user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex)


class DragBar(QWidget):
    """Верхня панель: тягнемо вікно, повзунок прозорості, Lock, закрити."""

    def __init__(self, win: "Overlay"):
        super().__init__(win)
        self.win = win
        self._press = None
        self._origin = None
        self.setFixedHeight(30)
        self.setStyleSheet("background: rgba(18,18,20,0.82);")

        lay = QHBoxLayout(self)
        lay.setContentsMargins(8, 0, 4, 0)
        lay.setSpacing(6)

        title = QLabel("Chat", self)
        title.setStyleSheet("color:#d4d4d8; font: bold 11px 'Segoe UI';")
        lay.addWidget(title)
        lay.addStretch(1)

        opacity = QSlider(Qt.Horizontal, self)
        opacity.setFixedWidth(80)
        opacity.setRange(20, 100)
        opacity.setValue(int(win.windowOpacity() * 100))
        opacity.valueChanged.connect(lambda v: win.setWindowOpacity(v / 100))
        opacity.setStyleSheet("QSlider{max-height:14px;}")
        lay.addWidget(opacity)

        self.lock_btn = QPushButton("🔓", self)
        self.lock_btn.setToolTip("Клік-крізь (Ctrl+Alt+Space)")
        self.lock_btn.setFixedSize(24, 22)
        self.lock_btn.clicked.connect(win.toggle_click_through)
        self.lock_btn.setStyleSheet(self._btn_css())
        lay.addWidget(self.lock_btn)

        close = QPushButton("✕", self)
        close.setFixedSize(24, 22)
        close.clicked.connect(win.close)
        close.setStyleSheet(self._btn_css("#ef4444"))
        lay.addWidget(close)

    def _btn_css(self, hover="#3f3f46"):
        return (
            "QPushButton{background:transparent;color:#d4d4d8;border:none;"
            "border-radius:4px;font:12px 'Segoe UI';}"
            f"QPushButton:hover{{background:{hover};color:#fff;}}"
        )

    def set_locked(self, locked: bool):
        self.lock_btn.setText("🔒" if locked else "🔓")

    def mousePressEvent(self, e):
        if e.button() == Qt.LeftButton:
            self._press = e.globalPosition().toPoint()
            self._origin = self.win.pos()

    def mouseMoveEvent(self, e):
        if self._press is not None:
            delta = e.globalPosition().toPoint() - self._press
            self.win.move(self._origin + delta)

    def mouseReleaseEvent(self, e):
        self._press = None
        self.win.save_config()


class Overlay(QMainWindow):
    def __init__(self, url: str):
        super().__init__()
        self.click_through = False

        # Рамкове/прозоре/поверх усіх/tool-вікно (без панелі задач).
        self.setWindowFlags(
            Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
        )
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setWindowTitle("Chat Overlay")

        central = QWidget(self)
        central.setAttribute(Qt.WA_TranslucentBackground, True)
        vbox = QVBoxLayout(central)
        vbox.setContentsMargins(0, 0, 0, 0)
        vbox.setSpacing(0)

        self.bar = DragBar(self)
        vbox.addWidget(self.bar)

        # Вебв'ю з прозорим фоном — сама сторінка /overlay/chat теж прозора.
        self.view = QWebEngineView(self)
        self.view.page().setBackgroundColor(QColor(0, 0, 0, 0))
        self.view.setAttribute(Qt.WA_TranslucentBackground, True)
        self.view.load(QUrl(url))
        vbox.addWidget(self.view, 1)

        self.setCentralWidget(central)

        # Куточок для зміни розміру (внизу праворуч).
        self.grip = QSizeGrip(central)
        self.grip.setFixedSize(16, 16)

        self._load_config()

    # --- розміщення size-grip у правому нижньому куті -----------------------
    def resizeEvent(self, e):
        super().resizeEvent(e)
        self.grip.move(self.width() - self.grip.width(), self.height() - self.grip.height())
        self.grip.raise_()
        self.save_config()

    def showEvent(self, e):
        super().showEvent(e)
        # Виключаємо вікно з захоплення екрана (OBS його не бачитиме).
        if not exclude_from_capture(self):
            print("[chat-overlay] УВАГА: не вдалося виключити з захоплення "
                  "(потрібна Windows 10 2004+/11). OBS може бачити вікно.")
        self._register_hotkey()

    # --- клік-крізь ---------------------------------------------------------
    def toggle_click_through(self):
        self.click_through = not self.click_through
        set_click_through(self, self.click_through)
        self.bar.set_locked(self.click_through)

    # --- глобальна гаряча клавіша Ctrl+Alt+Space ---------------------------
    def _register_hotkey(self):
        try:
            user32.RegisterHotKey(_hwnd(self), HOTKEY_ID,
                                  MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE)
        except Exception:
            pass

    def nativeEvent(self, eventType, message):
        if eventType == b"windows_generic_MSG":
            msg = wintypes.MSG.from_address(int(message))
            if msg.message == WM_HOTKEY and msg.wParam == HOTKEY_ID:
                self.toggle_click_through()
        return False, 0

    # --- збереження/відновлення конфігу ------------------------------------
    def _load_config(self):
        cfg = {}
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                cfg = json.load(f)
        except Exception:
            pass
        g = cfg.get("geometry")
        if g and all(k in g for k in ("x", "y", "w", "h")):
            self.setGeometry(g["x"], g["y"], g["w"], g["h"])
        else:
            self.resize(360, 560)
            self.move(60, 60)
        self.setWindowOpacity(cfg.get("opacity", 0.92))

    def save_config(self):
        cfg = {
            "geometry": {"x": self.x(), "y": self.y(), "w": self.width(), "h": self.height()},
            "opacity": round(self.windowOpacity(), 2),
        }
        try:
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump(cfg, f)
        except Exception:
            pass

    def closeEvent(self, e):
        self.save_config()
        try:
            user32.UnregisterHotKey(_hwnd(self), HOTKEY_ID)
        except Exception:
            pass
        super().closeEvent(e)


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else CHAT_URL
    # Прозорий фон вебв'ю коректніше працює з цим прапорцем.
    os.environ.setdefault("QTWEBENGINE_CHROMIUM_FLAGS", "--enable-features=TranslucentWindows")
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(True)
    win = Overlay(url)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    if sys.platform != "win32":
        print("Ця програма розрахована на Windows (виключення з захоплення екрана "
              "працює лише там).")
    main()
