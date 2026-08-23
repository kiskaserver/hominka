"""Обрамлення вікна: смужка для перетягування і куточок розміру.

Вікно без системної рамки (інакше заголовок Windows світився б у кадрі), тож
і тягнути, і міняти розмір доводиться самим.
"""

from typing import TYPE_CHECKING

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QPainter, QPen
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QPushButton, QSizeGrip

from ..styles import ACCENT_ACTIVE, ACCENT_LOCKED, BTN_CSS

if TYPE_CHECKING:                      # тільки для підказок типів
    from ..overlay import Overlay


class SizeGrip(QSizeGrip):
    """Помітний куточок для зміни розміру (три діагональні риски в акценті)."""

    def __init__(self, parent, accent: str):
        super().__init__(parent)
        self.accent = accent
        self.setFixedSize(22, 22)
        self.setToolTip("Тягни, щоб змінити розмір")

    def set_accent(self, c: str):
        self.accent = c
        self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        # ледь помітна підкладка, щоб куточок було видно на будь-якому фоні
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(0, 0, 0, 70))
        p.drawRoundedRect(self.rect().adjusted(4, 4, 0, 0), 5, 5)
        pen = QPen(QColor(self.accent))
        pen.setWidth(2)
        pen.setCapStyle(Qt.RoundCap)
        p.setPen(pen)
        w, h = self.width(), self.height()
        for off in (0, 5, 10):
            p.drawLine(w - 3, h - 13 + off, w - 13 + off, h - 3)


class DragBar(QFrame):
    """Верхня панель: тягнемо вікно + ⚙ налаштування + Lock + закрити."""

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

        self.title = QLabel("Chat", self)
        self.title.setStyleSheet("color:#fafafa; font:600 12px 'Segoe UI';")
        lay.addWidget(self.title)
        lay.addStretch(1)

        self.gear = QPushButton("⚙", self)
        self.gear.setToolTip("Налаштування: посилання, прозорість, тло, шрифт")
        self.gear.setFixedSize(26, 24)
        self.gear.setStyleSheet(BTN_CSS)
        self.gear.clicked.connect(win.toggle_settings)
        lay.addWidget(self.gear)

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

    def set_locked(self, locked: bool):
        self.lock_btn.setText("🔒" if locked else "🔓")
        c = ACCENT_LOCKED if locked else ACCENT_ACTIVE
        self.dot.setStyleSheet(f"color:{c}; font:12px 'Segoe UI';")

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
