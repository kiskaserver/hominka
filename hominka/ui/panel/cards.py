"""Картки панелі: «Чат», «Вигляд», «Оновлення».

Кожна картка — окремий розділ налаштувань, вони не знають одна про одну.
Живуть окремо від каркаса вікна саме тому, що змінюються найчастіше: додати
поле в «Чат» не повинно означати похід у код розміщення вікна.
"""

from typing import TYPE_CHECKING

from PySide6.QtCore import Qt, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QFrame, QHBoxLayout, QLabel, QLineEdit, QPushButton,
)

from ... import rtss as rtss_mod
from ... import update as updater
from ...fullscreen import changed_window as _changed_window
from ...version import APP_VERSION

if TYPE_CHECKING:                      # тільки для підказок типів
    from ...overlay import Overlay


class CardsMixin:
    """Побудова карток налаштувань і їхній стан. Частина SettingsPanel."""

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
    # --- секція «Поверх гри» -------------------------------------------------
    def _top_card(self) -> QFrame:
        """Що робити, коли гра йде на весь екран.

        Тут не налаштування, а відповідь на єдине питання, з яким сюди
        приходять: «чому чата не видно?». Тому спершу стан — що саме зараз
        попереду і чи побачить людина чат, — і лише потім кнопки.
        """
        card, lay = self._card("Поверх гри")

        self.top_status = QLabel("", self)
        self.top_status.setObjectName("dim")
        self.top_status.setWordWrap(True)
        lay.addWidget(self.top_status)

        row = QHBoxLayout()
        row.setSpacing(6)
        self.borderless_btn = QPushButton("Зробити гру безрамковою", self)
        self.borderless_btn.setObjectName("ghost")
        self.borderless_btn.setFixedHeight(28)
        self.borderless_btn.setToolTip(
            "Знімає з вікна гри рамку і розтягує на монітор. Гра виглядає так само, "
            "але малює її вже система — і чат поверх неї видно. Нічого в саму гру ми "
            "не встановлюємо.")
        self.borderless_btn.clicked.connect(self._make_borderless)
        row.addWidget(self.borderless_btn, 1)

        self.restore_btn = QPushButton("Повернути", self)
        self.restore_btn.setObjectName("ghost")
        self.restore_btn.setFixedHeight(28)
        self.restore_btn.setToolTip("Повернути вікну гри те, що в нього було.")
        self.restore_btn.clicked.connect(self._restore_window)
        self.restore_btn.hide()
        row.addWidget(self.restore_btn)
        lay.addLayout(row)

        # Дзеркало в RTSS: єдиний спосіб побачити чат у виключному
        # повноекранному режимі без інʼєкції в гру.
        #
        # Показуємо лише в тестових каналах. У стабільному каналі люди ведуть
        # ефіри, і експериментальні можливості мають доходити до них уже
        # перевіреними — а не «спробуйте, раптом спрацює».
        self.rtss = QCheckBox("Дублювати чат в RTSS (текстом)", self)
        self.rtss.setToolTip(
            "RivaTuner Statistics Server уже вміє малювати поверх гри — ми просто "
            "просимо його показати останні рядки чату. Без аватарок і емоутів, "
            "зате видно навіть у виключному повноекранному режимі.")
        self.rtss.toggled.connect(self._toggle_rtss)
        lay.addWidget(self.rtss)

        # Кнопка з'являється лише тоді, коли є що робити: запустити вже
        # встановлений RTSS або піти по нього.
        self.rtss_action = QPushButton("", self)
        self.rtss_action.setObjectName("ghost")
        self.rtss_action.setFixedHeight(26)
        self.rtss_action.clicked.connect(self._rtss_action)
        self.rtss_action.hide()
        lay.addWidget(self.rtss_action)

        if self.win.channel == "stable":
            self.rtss.hide()
            self.rtss_action.hide()

        self.keep_top = QCheckBox("Тримати поверх усіх вікон", self)
        self.keep_top.setToolTip(
            "У рідкісних старих іграх це дає мерехтіння — тоді вимкніть.")
        self.keep_top.toggled.connect(self.win.set_keep_top)
        lay.addWidget(self.keep_top)
        return card

    def _toggle_rtss(self, on: bool):
        """Вмикає дзеркало, а якщо RTSS немає — пропонує зробити наступний крок.

        Просто сказати «не працює» мало: людина не зобов'язана знати, що таке
        RTSS і де його брати.
        """
        if on and not self.win.rtss.available():
            self.rtss.blockSignals(True)
            self.rtss.setChecked(False)
            self.rtss.blockSignals(False)
            if rtss_mod.installed_path():
                self.top_status.setText("RTSS встановлено, але не запущено.")
                self.rtss_action.setText("Запустити RTSS")
            else:
                self.top_status.setText(
                    "Потрібен RivaTuner Statistics Server — безкоштовна програма, "
                    "яка вміє малювати поверх гри (йде разом із MSI Afterburner).")
                self.rtss_action.setText("Завантажити RTSS")
            self.rtss_action.show()
            return
        self.rtss_action.hide()
        self.win.set_rtss(on)

    def _rtss_action(self):
        """Запустити RTSS або відкрити сторінку завантаження."""
        if rtss_mod.installed_path():
            if rtss_mod.launch():
                self.top_status.setText("Запускаю RTSS… за кілька секунд увімкніть галочку ще раз.")
                self.rtss_action.hide()
            else:
                self.top_status.setText("Не вдалося запустити RTSS — спробуйте вручну.")
            return
        QDesktopServices.openUrl(QUrl(rtss_mod.RTSS_SITE))
        self.top_status.setText("Відкрив сторінку завантаження RTSS.")

    def set_fullscreen_state(self, info: dict):
        """Показує, що зараз попереду, простими словами."""
        kind = info.get("kind", "none")
        title = (info.get("title") or info.get("exe") or "").strip()
        short = title if len(title) <= 34 else title[:33] + "…"
        if kind in ("none", "desktop"):
            text = "Попереду немає гри — чат видно як завжди."
        elif kind == "exclusive":
            text = ("«%s» у виключному повноекранному режимі: поверх нього не малює "
                    "ніхто, крім самої гри. Натисніть кнопку нижче — вікно стане "
                    "безрамковим, і чат зʼявиться." % short)
        elif kind == "borderless":
            text = "«%s» на весь екран, але вікном — чат буде видно." % short
        else:
            text = "«%s» у вікні — чат буде видно." % short
        self.top_status.setText(text)
        self.borderless_btn.setEnabled(kind in ("windowed", "exclusive", "borderless"))
        self.restore_btn.setVisible(bool(self.win.__class__ and _changed_window()))

    def _make_borderless(self):
        if self.win.make_game_borderless():
            self.top_status.setText("Готово: вікно гри тепер безрамкове.")
            self.restore_btn.show()

    def _restore_window(self):
        if self.win.restore_game_window():
            self.top_status.setText("Повернули вікну гри те, що в нього було.")
            self.restore_btn.hide()

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
