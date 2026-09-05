"""Панель налаштувань — окреме вікно поруч із чатом.

Розкладено на три частини: тут каркас вікна (тінь, розміщення, спільні
обробники), у `cards.py` — самі картки з полями, у `widgets.py` — дрібні
деталі, з яких ці картки складають. Разом це був файл на 342 рядки, у якому
верстка трьох різних розділів налаштувань перемішана з їхньою поведінкою.
"""

from typing import TYPE_CHECKING

from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QAbstractSpinBox, QApplication, QComboBox, QFrame, QGraphicsDropShadowEffect,
    QHBoxLayout, QLabel, QPushButton, QScrollArea, QSlider, QVBoxLayout, QWidget,
)

from ...styles import PANEL_CSS, SLIDER_CSS
from ...version import APP_VERSION
from ...winapi import exclude_from_capture
from .cards import CardsMixin
from .widgets import WidgetsMixin

if TYPE_CHECKING:                      # тільки для підказок типів
    from ...overlay import Overlay


class _WheelGuard(QObject):
    """Колесо миші над комбобоксом/повзунком у прокрутці МАЄ гортати список, а
    не міняти значення під курсором.

    Стандартна пастка Qt: QComboBox/QSlider ловлять колесо навіть без фокуса, і
    прокрутка панелі фантомно перемикає опції чи совгає прозорість. Тому колесо
    над таким віджетом, коли він НЕ у фокусі, ми перенаправляємо у прокрутку, а
    до самого віджета не пускаємо. Клацнув, сфокусував — тоді крути на здоров'я.
    """

    def __init__(self, scroll: QScrollArea):
        super().__init__(scroll)
        self._scroll = scroll

    def eventFilter(self, obj, ev):
        if ev.type() == QEvent.Wheel and not obj.hasFocus():
            QApplication.sendEvent(self._scroll.viewport(), ev)
            return True                # до віджета не доходить — значення не міняється
        return False


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
        # Qt.Window разом із Qt.Tool — щоб вікно було таким самим top-level, як
        # головне вікно чату (воно ховається від захоплення надійно, а панель на
        # деяких Windows 10 — ні; єдина відмінність у прапорцях була саме Qt.Window).
        self.setWindowFlags(Qt.Window | Qt.Tool | Qt.FramelessWindowHint
                            | Qt.WindowStaysOnTopHint)
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

        body_lay = QVBoxLayout(self.body)
        body_lay.setContentsMargins(14, 12, 14, 14)
        body_lay.setSpacing(10)
        body_lay.addLayout(self._header())

        # Картки — у прокрутці: разом вони бувають вищі за екран (особливо коли
        # розкрито розділ гри), і тоді кнопки внизу просто не дотягтися. Прокрутка
        # тримає вікно в межах екрана, а хрестик і заголовок лишає завжди на очах.
        self.scroll = QScrollArea(self.body)
        self.scroll.setWidgetResizable(True)
        self.scroll.setFrameShape(QFrame.NoFrame)
        self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.scroll.setVerticalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        self.scroll.viewport().setAutoFillBackground(False)
        self.scroll.setStyleSheet(
            "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"
            "QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }"
            "QScrollBar::handle:vertical { background: rgba(255,255,255,0.18);"
            " border-radius: 4px; min-height: 30px; }"
            "QScrollBar::handle:vertical:hover { background: rgba(255,255,255,0.30); }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }")
        content = QWidget(self.scroll)
        content.setAttribute(Qt.WA_TranslucentBackground, True)
        lay = QVBoxLayout(content)
        lay.setContentsMargins(0, 0, 6, 0)   # місце під смужку прокрутки праворуч
        lay.setSpacing(10)
        lay.addWidget(self._chat_card())
        lay.addWidget(self._look_card())
        lay.addWidget(self._top_card())
        lay.addWidget(self._update_card())
        lay.addStretch(1)
        self.scroll.setWidget(content)
        self._content = content
        body_lay.addWidget(self.scroll)

        # Колесо над комбобоксами/повзунками у прокрутці більше не міняє їх
        # фантомно — гортає панель (див. _WheelGuard).
        self._wheel_guard = _WheelGuard(self.scroll)
        for cls in (QComboBox, QSlider, QAbstractSpinBox):
            for w in content.findChildren(cls):
                w.installEventFilter(self._wheel_guard)
                w.setFocusPolicy(Qt.StrongFocus)

        self.hide()

    def cap_height(self):
        """Обмежує висоту прокрутки висотою екрана — щоб вікно не вилазило за край.

        Рахуємо перед показом: без обмеження QScrollArea росте під увесь вміст,
        і сенсу в прокрутці немає.
        """
        scr = self.screen().availableGeometry() if self.screen() else None
        if not scr:
            return
        # Повна бажана висота вмісту.
        want = self._content.sizeHint().height()
        # Скільки лишається під картки: екран мінус тінь, заголовок, поля.
        chrome = self.SHADOW * 2 + 46
        avail = scr.height() - chrome - 8
        self.scroll.setMaximumHeight(max(200, min(want, avail)))

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
            self.cap_height()
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
        # На деяких Windows 10 приховування «не прилипає», якщо застосувати його
        # лише в мить показу (вікно ще не склалося композитором). Повторюємо кілька
        # разів після появи — дешево і надійно. На інших системах — нешкідливо.
        from PySide6.QtCore import QTimer
        for delay in (0, 60, 250):
            QTimer.singleShot(delay, lambda: exclude_from_capture(self))
