"""Панель налаштувань — окреме вікно поруч із чатом.

Розкладено на три частини: тут каркас вікна (тінь, розміщення, спільні
обробники), у `cards.py` — самі картки з полями, у `widgets.py` — дрібні
деталі, з яких ці картки складають. Разом це був файл на 342 рядки, у якому
верстка трьох різних розділів налаштувань перемішана з їхньою поведінкою.
"""

from typing import TYPE_CHECKING

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QFrame, QGraphicsDropShadowEffect, QHBoxLayout, QLabel, QPushButton, QVBoxLayout,
    QWidget,
)

from ...styles import PANEL_CSS, SLIDER_CSS
from ...version import APP_VERSION
from ...winapi import exclude_from_capture
from .cards import CardsMixin
from .widgets import WidgetsMixin

if TYPE_CHECKING:                      # тільки для підказок типів
    from ...overlay import Overlay


class SettingsPanel(CardsMixin, WidgetsMixin, QWidget):
    """Вікно налаштувань.

    Окреме верхнє вікно, а не панель усередині чату: нативний QWebEngineView
    малює поверх усього, що на ньому лежить, — від панелі було видно лише
    обрізаний край. І відкривається воно ЗБОКУ від чату, а не поверх нього:
    налаштування крутять саме тоді, коли читають чат, і затуляти його собою —
    те саме, що правити гучність, закривши екран.

    Оформлення: темна картка з тінню, всередині секції. Вікно без рамки, тож
    заголовок і хрестик малюємо самі.
    """

    WIDTH = 330
    SHADOW = 16          # поле навколо картки під тінь
    GAP = 10             # відступ від вікна чату

    def __init__(self, win: "Overlay"):
        super().__init__(None)
        self.win = win
        self.setWindowFlags(Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint)
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setFixedWidth(self.WIDTH + self.SHADOW * 2)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(self.SHADOW, self.SHADOW, self.SHADOW, self.SHADOW)

        self.body = QFrame(self)
        self.body.setObjectName("body")
        self.body.setStyleSheet(PANEL_CSS)
        shadow = QGraphicsDropShadowEffect(self)
        shadow.setBlurRadius(28)
        shadow.setColor(QColor(0, 0, 0, 190))
        shadow.setOffset(0, 6)
        self.body.setGraphicsEffect(shadow)
        outer.addWidget(self.body)

        lay = QVBoxLayout(self.body)
        lay.setContentsMargins(14, 12, 14, 14)
        lay.setSpacing(10)

        lay.addLayout(self._header())
        lay.addWidget(self._chat_card())
        lay.addWidget(self._look_card())
        lay.addWidget(self._update_card())

        self.hide()

    # --- шапка ---------------------------------------------------------------
    def _header(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(6)
        title = QLabel("Налаштування", self)
        title.setObjectName("title")
        row.addWidget(title)
        row.addStretch(1)
        ver = QLabel(APP_VERSION, self)
        ver.setObjectName("dim")
        row.addWidget(ver)
        close = QPushButton("\u2715", self)
        close.setObjectName("ghost")
        close.setFixedSize(24, 24)
        close.setToolTip("Закрити налаштування")
        close.clicked.connect(self.hide)
        row.addWidget(close)
        return row

    # --- поведінка -----------------------------------------------------------
    def _on_channel(self, _idx: int):
        self.win.set_channel(self.channel.currentData())

    def _on_auto(self, on: bool):
        self.win.auto_update = bool(on)
        self.win.save_config()

    def set_status(self, text: str):
        """Текст про оновлення. Панель під нього ПІДРОСТАЄ.

        Без цього довгий опис змін просто обрізався: вікно вже показане, а
        його висота порахована для короткого рядка.
        """
        self.upd_status.setText(text)
        if self.isVisible():
            self.adjustSize()
            self.win._place_panel()

    def _apply_channel(self):
        self.win.set_my_channel(self.channel_edit.text())

    def _apply_extra(self):
        self.win.set_extra_channels(self.twitch_edit.text(), self.kick_edit.text())

    def _apply_site(self):
        self.win.set_site_url(self.site_edit.text())

    def set_chat_error(self, text: str):
        self.chat_error.setText(text)
        self.chat_error.setVisible(bool(text))

    def _on_delay(self, v):
        self.delay_val.setText("%d с" % v)
        self.win.set_chat_delay(v)

    def _on_opacity(self, v):
        self.win.setWindowOpacity(v / 100)
        self.op_pct.setText(f"{v}%")
        self.win.save_config()

    def _on_bg(self, v):
        self.bg_pct.setText(f"{v}%")
        self.win.set_bg_alpha(v)

    def set_accent(self, c: str):
        self.opacity.setStyleSheet(SLIDER_CSS % c)
        self.bg.setStyleSheet(SLIDER_CSS % c)

    def sync_zoom(self):
        self.zoom_lbl.setText(f"{int(self.win.zoom * 100)}%")

    def showEvent(self, e):
        super().showEvent(e)
        exclude_from_capture(self)  # OBS не бачить і вікно налаштувань
