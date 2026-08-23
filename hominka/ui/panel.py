"""Панель налаштувань — окреме вікно поруч із чатом."""

from typing import TYPE_CHECKING

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QFrame, QGraphicsDropShadowEffect, QHBoxLayout, QLabel,
    QLineEdit, QPushButton, QSlider, QVBoxLayout, QWidget,
)

from .. import updater
from ..styles import ACCENT_ACTIVE, PANEL_CSS, SLIDER_CSS
from ..winapi import exclude_from_capture
from ..version import APP_VERSION

if TYPE_CHECKING:                      # тільки для підказок типів
    from ..overlay import Overlay


class SettingsPanel(QWidget):
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

    # --- секція «Чат» --------------------------------------------------------
    def _chat_card(self) -> QFrame:
        card, lay = self._card("Чат")

        lay.addWidget(self._label("YouTube"))
        self.channel_edit = QLineEdit(self)
        self.channel_edit.setPlaceholderText("@нік, посилання на канал або UC…")
        self.channel_edit.returnPressed.connect(self._apply_channel)
        self.channel_edit.editingFinished.connect(self._apply_channel)
        lay.addWidget(self.channel_edit)

        lay.addWidget(self._label("Twitch"))
        self.twitch_edit = QLineEdit(self)
        self.twitch_edit.setPlaceholderText("twitch.tv/канал або просто нік")
        self.twitch_edit.returnPressed.connect(self._apply_extra)
        self.twitch_edit.editingFinished.connect(self._apply_extra)
        lay.addWidget(self.twitch_edit)

        lay.addWidget(self._label("Kick"))
        self.kick_edit = QLineEdit(self)
        self.kick_edit.setPlaceholderText("kick.com/канал або просто нік")
        self.kick_edit.returnPressed.connect(self._apply_extra)
        self.kick_edit.editingFinished.connect(self._apply_extra)
        lay.addWidget(self.kick_edit)

        lay.addWidget(self._label("Свій чат за посиланням"))
        self.site_edit = QLineEdit(self)
        self.site_edit.setPlaceholderText("не обов'язково — сторінка чату")
        self.site_edit.setToolTip(
            "Якщо у вас свій сайт зі своїм чатом — вставте сюди посилання на "
            "його сторінку. Для звичайних площадок це поле не потрібне: "
            "досить назвати канал вище.")
        self.site_edit.returnPressed.connect(self._apply_site)
        self.site_edit.editingFinished.connect(self._apply_site)
        lay.addWidget(self.site_edit)

        self.src_status = QLabel("", self)
        self.src_status.setObjectName("dim")
        self.src_status.setWordWrap(True)
        lay.addWidget(self.src_status)

        # Місце для скарг читачів: коли каналу не існує або площадка не
        # відповідає, це має бути видно тут же, а не тільки в порожньому вікні.
        self.chat_error = QLabel("", self)
        self.chat_error.setObjectName("error")
        self.chat_error.setWordWrap(True)
        self.chat_error.hide()
        lay.addWidget(self.chat_error)

        lay.addSpacing(2)
        lay.addWidget(self._label("Затримка чату"))
        self.delay, self.delay_val = self._slider_row(
            lay, 0, 60, int(self.win.chat_delay), self._on_delay, suffix=" с")
        hint2 = QLabel("Тримає повідомлення й видає їх по одному — коли пишуть "
                       "швидше, ніж читаєш, стрічка перестає бути кашею. "
                       "0 — без затримки.", self)
        hint2.setObjectName("dim")
        hint2.setWordWrap(True)
        lay.addWidget(hint2)
        return card

    # --- секція «Вигляд» -----------------------------------------------------
    def _look_card(self) -> QFrame:
        card, lay = self._card("Вигляд")

        lay.addWidget(self._label("Прозорість вікна"))
        self.opacity, self.op_pct = self._slider_row(
            lay, 25, 100, int(self.win.windowOpacity() * 100), self._on_opacity)

        lay.addWidget(self._label("Тло під чатом"))
        self.bg, self.bg_pct = self._slider_row(
            lay, 0, 100, int(self.win.bg_alpha * 100), self._on_bg)

        lay.addWidget(self._label("Розмір тексту"))
        row = QHBoxLayout()
        row.setSpacing(6)
        minus = QPushButton("A\u2212", self)
        minus.setObjectName("ghost")
        minus.setFixedSize(38, 28)
        minus.clicked.connect(self.win.zoom_out)
        row.addWidget(minus)
        self.zoom_lbl = QLabel(f"{int(self.win.zoom * 100)}%", self)
        self.zoom_lbl.setObjectName("value")
        self.zoom_lbl.setAlignment(Qt.AlignCenter)
        self.zoom_lbl.setFixedWidth(52)
        row.addWidget(self.zoom_lbl)
        plus = QPushButton("A+", self)
        plus.setObjectName("ghost")
        plus.setFixedSize(38, 28)
        plus.clicked.connect(self.win.zoom_in)
        row.addWidget(plus)
        row.addStretch(1)
        lay.addLayout(row)

        # Свій CSS — окремим вікном: у полі на три сантиметри код не пишуть.
        lay.addSpacing(2)
        css = QPushButton("Свій CSS для чату…", self)
        css.setObjectName("ghost")
        css.setFixedHeight(28)
        css.setToolTip("Повноцінний редактор: приклад чату поруч, довідник класів, "
                       "перевірка синтаксису. Від захоплення екрана вікно теж сховане.")
        css.clicked.connect(self.win.open_css_editor)
        lay.addWidget(css)
        return card

    # --- секція «Оновлення» --------------------------------------------------
    def _update_card(self) -> QFrame:
        card, lay = self._card("Оновлення")

        row = QHBoxLayout()
        row.setSpacing(6)
        self.channel = QComboBox(self)
        for cid, label, tip in updater.CHANNELS:
            self.channel.addItem(label, cid)
            self.channel.setItemData(self.channel.count() - 1, tip, Qt.ToolTipRole)
        self.channel.currentIndexChanged.connect(self._on_channel)
        row.addWidget(self.channel, 1)
        check = QPushButton("Перевірити", self)
        check.setObjectName("ghost")
        check.setFixedHeight(28)
        check.clicked.connect(lambda: self.win.check_updates(manual=True))
        row.addWidget(check)
        lay.addLayout(row)

        self.auto_upd = QCheckBox("Перевіряти автоматично", self)
        self.auto_upd.toggled.connect(self._on_auto)
        lay.addWidget(self.auto_upd)

        self.upd_status = QLabel(f"Версія {APP_VERSION}", self)
        self.upd_status.setObjectName("dim")
        self.upd_status.setWordWrap(True)
        self.upd_status.setTextFormat(Qt.RichText)
        lay.addWidget(self.upd_status)
        return card

    # --- будівельні дрібниці -------------------------------------------------
    def _card(self, title: str):
        card = QFrame(self)
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(11, 9, 11, 11)
        lay.setSpacing(5)
        cap = QLabel(title, card)
        cap.setObjectName("cap")
        lay.addWidget(cap)
        return card, lay

    def _label(self, text: str) -> QLabel:
        lab = QLabel(text, self)
        lab.setObjectName("field")
        return lab

    def _slider_row(self, parent_lay, lo, hi, val, cb, suffix="%"):
        row = QHBoxLayout()
        row.setSpacing(8)
        sld = QSlider(Qt.Horizontal, self)
        sld.setRange(lo, hi)
        sld.setValue(val)
        sld.setStyleSheet(SLIDER_CSS % ACCENT_ACTIVE)
        sld.valueChanged.connect(cb)
        row.addWidget(sld, 1)
        pct = QLabel(f"{val}{suffix}", self)
        pct.setObjectName("value")
        pct.setFixedWidth(40)
        pct.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        row.addWidget(pct)
        parent_lay.addLayout(row)
        return sld, pct

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

    def set_source_status(self, win: "Overlay"):
        """Показує, який канал знайдено і що саме зараз у вікні.

        Без цього автоматика мовчазна: незрозуміло, чому чат такий, а не інший,
        і що зробити, щоб став іншим.
        """
        typed = self.channel_edit.text().strip()
        known = win.yt_channel_title or win.yt_channel_id

        if typed and not known:
            head = "Такого каналу не знайшли — перевірте @нік чи посилання."
        elif known:
            head = "Канал: %s." % known
        else:
            head = "Канал YouTube не вказано."

        site = bool(win.site_url.strip())
        if win.mode == "feed":
            tail = "Читаємо: %s." % ", ".join(win.active_sources())
        elif win.auto_video:
            tail = "Ефір іде — показуємо його чат."
        elif known and site:
            tail = "Ефіру немає — показуємо чат сайту, перемкнемось самі, щойно почнеться."
        elif known:
            tail = "Ефіру немає, а посилання на чат сайту не вписано — вікно порожнє."
        elif site:
            tail = "Показуємо чат сайту."
        else:
            tail = "Джерела немає: вставте посилання на чат сайту або назвіть канал."
        self.src_status.setText(head + " " + tail)

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
