"""
Chat Overlay — прозорий оверлей чату поверх гри, НЕВИДИМИЙ для OBS.

Навіщо: у тебе один монітор, і чат, який OBS накладає на трансляцію, ти сам не
бачиш. Ця програма показує твою сторінку чату (/overlay/chat) окремим вікном
поверх усіх ігор, але Windows приховує це вікно від будь-якого захоплення екрана
(OBS Display/Window/Game Capture його НЕ бачить — WDA_EXCLUDEFROMCAPTURE,
Windows 10 2004+ / Windows 11).

Можливості:
  • рамка навколо — видно, де вікно на екрані (фіолетова = звичайний режим,
    зелена = клік-крізь);
  • перетягування — за верхню панель;
  • зміна розміру — за куточок унизу праворуч (або край);
  • прозорість — повзунок на панелі (зі значенням у %);
  • клік-крізь (миша йде в гру) — кнопка або Ctrl+Alt+Space;
  • запам'ятовує розмір/позицію/прозорість (config.json).

Запуск:  python chat_overlay.py  [URL]
"""

import ctypes
import json
import os
import sys
from ctypes import wintypes

from PySide6.QtCore import Qt, QUrl  # noqa
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QFrame, QVBoxLayout, QHBoxLayout,
    QSizeGrip, QPushButton, QLabel, QSlider,
)
from PySide6.QtWebEngineWidgets import QWebEngineView

# === Налаштування за замовчуванням ==========================================
CHAT_URL = "https://stream.svitix.com/overlay/chat?lang=uk"

CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json")

ACCENT_ACTIVE = "#a855f7"   # рамка у звичайному режимі (фіолетова)
ACCENT_LOCKED = "#22c55e"   # рамка у режимі клік-крізь (зелена)

# === WinAPI константи =======================================================
WDA_EXCLUDEFROMCAPTURE = 0x00000011
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
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
    try:
        return bool(user32.SetWindowDisplayAffinity(_hwnd(win), WDA_EXCLUDEFROMCAPTURE))
    except Exception:
        return False


def set_click_through(win, enabled: bool):
    hwnd = _hwnd(win)
    ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    if enabled:
        ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED
    else:
        ex &= ~WS_EX_TRANSPARENT
    user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex)


BTN_CSS = """
QPushButton {
    background: rgba(255,255,255,0.06);
    color: #e4e4e7;
    border: none;
    border-radius: 6px;
    font: 12px 'Segoe UI';
}
QPushButton:hover { background: rgba(255,255,255,0.16); color: #fff; }
QPushButton:pressed { background: rgba(255,255,255,0.24); }
"""

SLIDER_CSS = """
QSlider { max-height: 16px; }
QSlider::groove:horizontal {
    height: 4px; border-radius: 2px; background: rgba(255,255,255,0.18);
}
QSlider::sub-page:horizontal { background: %s; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 12px; height: 12px; margin: -5px 0; border-radius: 6px;
    background: #ffffff;
}
"""


class DragBar(QFrame):
    """Верхня панель: тягнемо вікно + прозорість + Lock + закрити."""

    def __init__(self, win: "Overlay"):
        super().__init__(win)
        self.win = win
        self._press = None
        self._origin = None
        self.setObjectName("bar")
        self.setFixedHeight(34)
        self.setStyleSheet(
            "#bar { background: rgba(20,20,24,0.92);"
            " border-top-left-radius: 9px; border-top-right-radius: 9px;"
            " border-bottom: 1px solid rgba(255,255,255,0.08); }"
        )

        lay = QHBoxLayout(self)
        lay.setContentsMargins(10, 0, 6, 0)
        lay.setSpacing(7)

        self.dot = QLabel("●", self)
        self.dot.setStyleSheet(f"color:{ACCENT_ACTIVE}; font:12px 'Segoe UI';")
        lay.addWidget(self.dot)

        title = QLabel("Chat", self)
        title.setStyleSheet("color:#fafafa; font:600 12px 'Segoe UI';")
        lay.addWidget(title)
        lay.addStretch(1)

        op_icon = QLabel("◐", self)
        op_icon.setStyleSheet("color:#a1a1aa; font:12px 'Segoe UI';")
        op_icon.setToolTip("Прозорість")
        lay.addWidget(op_icon)

        self.opacity = QSlider(Qt.Horizontal, self)
        self.opacity.setFixedWidth(84)
        self.opacity.setRange(25, 100)
        self.opacity.setValue(int(win.windowOpacity() * 100))
        self.opacity.setStyleSheet(SLIDER_CSS % ACCENT_ACTIVE)
        self.opacity.valueChanged.connect(self._on_opacity)
        lay.addWidget(self.opacity)

        self.pct = QLabel(f"{self.opacity.value()}%", self)
        self.pct.setFixedWidth(34)
        self.pct.setStyleSheet("color:#a1a1aa; font:11px 'Segoe UI';")
        lay.addWidget(self.pct)

        self.lock_btn = QPushButton("🔓", self)
        self.lock_btn.setToolTip("Клік-крізь: миша піде в гру (Ctrl+Alt+Space)")
        self.lock_btn.setFixedSize(26, 24)
        self.lock_btn.setStyleSheet(BTN_CSS)
        self.lock_btn.clicked.connect(win.toggle_click_through)
        lay.addWidget(self.lock_btn)

        close = QPushButton("✕", self)
        close.setToolTip("Закрити")
        close.setFixedSize(26, 24)
        close.setStyleSheet(BTN_CSS + "QPushButton:hover{background:#dc2626;color:#fff;}")
        close.clicked.connect(win.close)
        lay.addWidget(close)

    def _on_opacity(self, v):
        self.win.setWindowOpacity(v / 100)
        self.pct.setText(f"{v}%")

    def set_locked(self, locked: bool):
        self.lock_btn.setText("🔒" if locked else "🔓")
        c = ACCENT_LOCKED if locked else ACCENT_ACTIVE
        self.dot.setStyleSheet(f"color:{c}; font:12px 'Segoe UI';")
        self.opacity.setStyleSheet(SLIDER_CSS % c)

    # перетягування вікна за панель
    def mousePressEvent(self, e):
        if e.button() == Qt.LeftButton:
            self._press = e.globalPosition().toPoint()
            self._origin = self.win.pos()

    def mouseMoveEvent(self, e):
        if self._press is not None:
            self.win.move(self._origin + (e.globalPosition().toPoint() - self._press))

    def mouseReleaseEvent(self, e):
        self._press = None
        self.win.save_config()


class Overlay(QMainWindow):
    def __init__(self, url: str):
        super().__init__()
        self.click_through = False

        self.setWindowFlags(
            Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
        )
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setWindowTitle("Chat Overlay")

        # Рамка навколо всього — видно, де вікно; колір показує режим.
        self.frame = QFrame(self)
        self.frame.setObjectName("frame")
        self._apply_border(ACCENT_ACTIVE)

        vbox = QVBoxLayout(self.frame)
        vbox.setContentsMargins(3, 3, 3, 3)
        vbox.setSpacing(0)

        self.bar = DragBar(self)
        vbox.addWidget(self.bar)

        self.view = QWebEngineView(self)
        self.view.page().setBackgroundColor(QColor(0, 0, 0, 0))
        self.view.setAttribute(Qt.WA_TranslucentBackground, True)
        self.view.load(QUrl(url))
        vbox.addWidget(self.view, 1)

        self.setCentralWidget(self.frame)

        self.grip = QSizeGrip(self.frame)
        self.grip.setFixedSize(18, 18)
        self.grip.setStyleSheet("background: transparent;")

        self._load_config()

    def _apply_border(self, accent: str):
        # Напівпрозорий темний фон під чатом (краще видно на світлих іграх) + рамка.
        self.frame.setStyleSheet(
            "#frame {"
            " background: rgba(12,12,15,0.30);"
            f" border: 2px solid {accent};"
            " border-radius: 11px;"
            " }"
        )

    def resizeEvent(self, e):
        super().resizeEvent(e)
        self.grip.move(self.width() - self.grip.width() - 3,
                       self.height() - self.grip.height() - 3)
        self.grip.raise_()
        self.save_config()

    def showEvent(self, e):
        super().showEvent(e)
        if not exclude_from_capture(self):
            print("[chat-overlay] УВАГА: не вдалося виключити з захоплення "
                  "(потрібна Windows 10 2004+/11). OBS може бачити вікно.")
        self._register_hotkey()

    def toggle_click_through(self):
        self.click_through = not self.click_through
        set_click_through(self, self.click_through)
        self.bar.set_locked(self.click_through)
        self._apply_border(ACCENT_LOCKED if self.click_through else ACCENT_ACTIVE)

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
        op = cfg.get("opacity", 0.94)
        self.setWindowOpacity(op)
        self.bar.opacity.setValue(int(op * 100))

    def save_config(self):
        try:
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump({
                    "geometry": {"x": self.x(), "y": self.y(), "w": self.width(), "h": self.height()},
                    "opacity": round(self.windowOpacity(), 2),
                }, f)
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
