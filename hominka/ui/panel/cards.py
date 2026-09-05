"""Картки панелі: «Чат», «Вигляд», «Оновлення».

Кожна картка — окремий розділ налаштувань, вони не знають одна про одну.
Живуть окремо від каркаса вікна саме тому, що змінюються найчастіше: додати
поле в «Чат» не повинно означати похід у код розміщення вікна.
"""

from typing import TYPE_CHECKING

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QFrame, QHBoxLayout, QLabel, QLineEdit, QPushButton,
    QSlider,
)

from ... import fullscreen as fs_mod
from ... import inject as inject_mod
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

        # Вибір вікна гри списком, а не автовизначенням переднього вікна.
        # Автодетект ловив будь-що, що опинилося попереду (зокрема нашу ж
        # панель під час Alt-Tab), і людина не бачила, що саме зробить
        # безрамковим. Список — той самий, що й для чату в грі
        # (fullscreen.list_windows): видимі вікна ігор, поруч кнопка оновити.
        bpick_row = QHBoxLayout()
        bpick_row.setSpacing(6)
        self.border_pick = QComboBox(self)
        self.border_pick.setToolTip("Оберіть вікно гри, яке зробити безрамковим.")
        # Не даємо довгим рядкам списку розсувати панель: ширину комбобокса
        # рахуємо від кількох символів, а не від найдовшого пункту (той
        # показується у випадайці; в самому полі — з трьома крапками).
        self.border_pick.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.border_pick.setMinimumContentsLength(6)
        bpick_row.addWidget(self.border_pick, 1)
        self.border_refresh = QPushButton("⟳", self)
        self.border_refresh.setObjectName("ghost")
        self.border_refresh.setFixedSize(30, 28)
        self.border_refresh.setToolTip("Оновити список вікон.")
        self.border_refresh.clicked.connect(self._refresh_borderless)
        bpick_row.addWidget(self.border_refresh)
        lay.addLayout(bpick_row)

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
        self.borderless_btn.setEnabled(False)   # доки не оберуть вікно зі списку
        row.addWidget(self.borderless_btn, 1)

        self.restore_btn = QPushButton("Повернути", self)
        self.restore_btn.setObjectName("ghost")
        self.restore_btn.setFixedHeight(28)
        self.restore_btn.setToolTip("Повернути вікну гри те, що в нього було.")
        self.restore_btn.clicked.connect(self._restore_window)
        self.restore_btn.hide()
        row.addWidget(self.restore_btn)
        lay.addLayout(row)


        # --- справжній чат у грі (інжектор) ---------------------------------
        # Найпотужніше і найризикованіше: своя бібліотека всередині процесу гри
        # малює справжній чат — з аватарками й емоутами — навіть у виключному
        # повноекранному режимі. Тільки тестові канали, і з чесним попередженням.
        self.game_box = QCheckBox("Справжній чат у грі (для одиночних ігор)", self)
        self.game_box.setToolTip(
            "Показує повний чат — з аватарками й емоутами — поверх гри, навіть "
            "коли вона у виключному повноекранному режимі. Вкладає бібліотеку в "
            "процес гри — на свій страх і ризик.")
        self.game_box.toggled.connect(self._toggle_game)
        lay.addWidget(self.game_box)

        # Спокійне бурштинове попередження (не «стіна червоного»): суть коротко,
        # деталі — за кнопкою нижче.
        self.game_warn = QLabel(
            "Чат малюється всередині гри — це надійно для одиночних ігор. "
            "Бібліотека непідписана, тож Windows (SmartScreen/Defender) може "
            "перепитати. В ОНЛАЙН-іграх з античитом так робити НЕ можна — там "
            "лишається безрамковий режим вище.", self)
        self.game_warn.setWordWrap(True)
        self.game_warn.setStyleSheet(
            "color:#fcd9a5; background:rgba(217,119,6,0.12);"
            "border:1px solid rgba(217,119,6,0.40); border-radius:6px; padding:6px 8px;")
        self.game_warn.hide()
        lay.addWidget(self.game_warn)

        # Кнопка з детальним поясненням: що саме перепитає Windows, що і як
        # (не)вимикати, і де інжект недопустимий.
        self.game_help_btn = QPushButton("ℹ Що потрібно, щоб чат у грі запрацював", self)
        self.game_help_btn.setObjectName("ghost")
        self.game_help_btn.setFixedHeight(26)
        self.game_help_btn.clicked.connect(self._show_injector_help)
        lay.addWidget(self.game_help_btn)

        # Вибір гри списком, а не «встигни перейти за 4 секунди»: панель
        # зникала разом з Alt-Tab, і натиснути було нікуди. Список — з видимих
        # вікон (fullscreen.list_windows), поруч кнопка оновити.
        pick_row = QHBoxLayout()
        pick_row.setSpacing(6)
        self.game_pick = QComboBox(self)
        self.game_pick.setToolTip("Оберіть вікно гри, у яке показати чат.")
        self.game_pick.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.game_pick.setMinimumContentsLength(6)
        pick_row.addWidget(self.game_pick, 1)
        self.game_refresh = QPushButton("⟳", self)
        self.game_refresh.setObjectName("ghost")
        self.game_refresh.setFixedSize(30, 28)
        self.game_refresh.setToolTip("Оновити список вікон.")
        self.game_refresh.clicked.connect(self._refresh_games)
        pick_row.addWidget(self.game_refresh)
        self.game_pick_row = pick_row
        lay.addLayout(pick_row)

        self.game_inject = QPushButton("Показати чат у грі", self)
        self.game_inject.setObjectName("ghost")
        self.game_inject.setFixedHeight(28)
        self.game_inject.setToolTip("Вкладе чат у вибрану гру.")
        self.game_inject.clicked.connect(self._inject_selected)
        lay.addWidget(self.game_inject)

        # Окремий рядок стану саме для інжектора — щоб його не затирав напис про
        # повноекранний режим (той живе у top_status і оновлюється таймером).
        self.game_status = QLabel("", self)
        self.game_status.setObjectName("dim")
        self.game_status.setWordWrap(True)
        lay.addWidget(self.game_status)

        # Позицію й розмір чату в грі задає САМЕ ВІКНО чату: пояснюємо це, щоб
        # людина не шукала повзунків «куди» — вона просто рухає й тягне вікно.
        self.game_place_hint = QLabel(
            "📍 Де стоїть і як розтягнуте це вікно чату на моніторі — там і "
            "такого ж розміру буде чат у грі. Пересунь/розтягни вікно чату — і "
            "чат у грі стане на те саме місце.", self)
        self.game_place_hint.setObjectName("dim")
        self.game_place_hint.setWordWrap(True)
        lay.addWidget(self.game_place_hint)

        op_row = QHBoxLayout()
        op_row.setSpacing(6)
        self.game_op_lbl = QLabel("Прозорість:", self)
        op_row.addWidget(self.game_op_lbl)
        self.game_opacity = QSlider(Qt.Horizontal, self)
        self.game_opacity.setRange(30, 255)
        self.game_opacity.setValue(int(self.win.game_opacity))
        self.game_opacity.valueChanged.connect(
            lambda v: self.win.set_game_opacity(v))
        op_row.addWidget(self.game_opacity, 1)
        lay.addLayout(op_row)

        # Ховати чат від OBS: стрімер бачить чат у грі на своєму моніторі, а
        # захоплення OBS знімає чистий кадр (чат не потрапляє в ефір).
        self.game_hide_obs = QCheckBox("Ховати чат від OBS (видно лише мені)", self)
        self.game_hide_obs.setChecked(bool(self.win.game_hide_obs))
        self.game_hide_obs.toggled.connect(self.win.set_game_hide_obs)
        lay.addWidget(self.game_hide_obs)
        self.game_hide_obs_hint = QLabel(
            "Працює на всіх підтримуваних API — DirectX 9, 11 і 12, OpenGL та "
            "Vulkan: чат лягає в кадр так, що OBS знімає його чистим — у грі видно, "
            "а в ефір не потрапляє.", self)
        self.game_hide_obs_hint.setObjectName("dim")
        self.game_hide_obs_hint.setWordWrap(True)
        lay.addWidget(self.game_hide_obs_hint)

        # Показуємо/ховаємо всю секцію одним списком.
        self._game_widgets = (self.game_warn, self.game_help_btn,
                              self.game_pick, self.game_refresh,
                              self.game_inject, self.game_status,
                              self.game_place_hint,
                              self.game_op_lbl, self.game_opacity,
                              self.game_hide_obs, self.game_hide_obs_hint)
        for w in self._game_widgets:
            w.hide()
        self._injected = set()   # hwnd, куди вже вкладено — щоб не інжектити двічі

        self.set_game_experimental(True)   # доступно в усіх каналах

        self.keep_top = QCheckBox("Тримати поверх усіх вікон", self)
        self.keep_top.setToolTip(
            "У рідкісних старих іграх це дає мерехтіння — тоді вимкніть.")
        self.keep_top.toggled.connect(self.win.set_keep_top)
        lay.addWidget(self.keep_top)

        self._refresh_borderless()     # наповнити список вікон одразу
        return card

    # --- інжектор чату в гру ---
    def set_game_experimental(self, on: bool):
        """Показує розділ інжектора, коли поруч є нативні файли (усі канали)."""
        show = on and inject_mod.available()
        self.game_box.setVisible(show)
        if not show:
            for w in self._game_widgets:
                w.hide()
            if self.game_box.isChecked():
                self.game_box.blockSignals(True)
                self.game_box.setChecked(False)
                self.game_box.blockSignals(False)

    def _toggle_game(self, on: bool):
        """Розкриває попередження, список ігор і кнопку; вимикає — гасить продюсера."""
        for w in self._game_widgets:
            w.setVisible(on)
        # Вміст змінився — переміряти висоту й переставити вікно, щоб воно
        # виросло (чи стислося) в межах екрана, а не лишалося старого розміру.
        if hasattr(self, "cap_height"):
            self.cap_height()
            self.adjustSize()
            self.win._place_panel()
        if on:
            self._refresh_games()
            # Розділ гри високий: прокручуємо до нього, щоб кнопка «Показати чат
            # у грі» була на очах, а не за нижнім краєм екрана.
            if hasattr(self, "scroll"):
                QTimer.singleShot(0, lambda: self.scroll.ensureWidgetVisible(self.game_inject))
        else:
            self.win.set_game_overlay(False)
            self._injected.clear()
            self.game_inject.setText("Показати чат у грі")
            self.game_status.setText("")

    def _refresh_games(self):
        """Наповнює список видимими вікнами. Кожен рядок памʼятає свій hwnd."""
        self.game_pick.clear()
        wins = fs_mod.list_windows()
        if not wins:
            self.game_pick.addItem("Немає відкритих ігор — запустіть гру й оновіть", None)
            self.game_inject.setEnabled(False)
            return
        for hwnd, pid, exe, title in wins:
            label = "%s — %s" % (title[:40], exe)
            self.game_pick.addItem(label, hwnd)
        self.game_inject.setEnabled(True)

    def _inject_selected(self):
        """Вкладає чат у вибрану зі списку гру (або каже, що вже вкладено)."""
        hwnd = self.game_pick.currentData()
        if not hwnd:
            self.game_status.setText("Спершу оберіть гру зі списку (⟳ оновлює).")
            return
        # Не інжектимо повторно в ту саму гру: DLL уже всередині, друга інʼєкція
        # нічого не дає, лише плодить зайві виклики.
        if hwnd in self._injected and inject_mod._pid_of(hwnd):
            self.game_status.setText("У цю гру чат уже вкладено.")
            return
        self.game_status.setText("Вкладаю чат…")
        res = self.win.inject_game(hwnd)
        ov = getattr(self.win, "game_overlay", None)
        if res.ok and ov is not None and getattr(ov.writer, "conflict", False):
            self.game_status.setText(
                "Інша копія Hominka вже показує чат у грі. Лишіть одну — двоє "
                "малюють одне поверх одного.")
            return
        if res.ok:
            exe = self.game_pick.currentText().split(" — ")[-1]
            self._injected.add(hwnd)
            self.game_status.setText("✓ Чат у грі працює: %s. Тут його й видно." % exe)
            self.game_inject.setText("Показати ще раз")
        else:
            self.game_status.setText(res.message)
        # Якщо гру заблокував античит — знімаємо галочку: тут інжектор не варіант.
        if res.code == inject_mod.EX_BLOCKED:
            self.game_box.blockSignals(True)
            self.game_box.setChecked(False)
            self.game_box.blockSignals(False)
            self._toggle_game(False)

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
        # Кнопку вмикає ВИБІР зі списку (_refresh_borderless), а не те, що зараз
        # попереду: людина сама каже, яке вікно чіпати.
        self.restore_btn.setVisible(bool(_changed_window()))

    def _show_injector_help(self):
        """Вікно з детальним поясненням: SmartScreen/Defender, що (не) вимикати,
        де інжект недопустимий."""
        from ..injector_help import InjectorHelpDialog
        dlg = InjectorHelpDialog(self.win)
        dlg.exec()

    def _refresh_borderless(self):
        """Наповнює список вікон для безрамкового режиму (той самий, що для чату)."""
        self.border_pick.clear()
        wins = fs_mod.list_windows()
        if not wins:
            self.border_pick.addItem("Немає відкритих ігор — запустіть гру й оновіть", None)
            self.borderless_btn.setEnabled(False)
            return
        for hwnd, pid, exe, title in wins:
            self.border_pick.addItem("%s — %s" % (title[:40], exe), hwnd)
        self.borderless_btn.setEnabled(True)

    def _make_borderless(self):
        hwnd = self.border_pick.currentData()
        if not hwnd:
            self.top_status.setText("Спершу оберіть вікно гри зі списку (⟳ оновлює).")
            return
        if self.win.make_game_borderless(hwnd):
            self.top_status.setText("Готово: вікно гри тепер безрамкове.")
            self.restore_btn.show()
        else:
            self.top_status.setText("Не вдалося змінити це вікно "
                                    "(уже безрамкове або зникло).")

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
