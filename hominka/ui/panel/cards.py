"""Картки панелі: «Чат», «Вигляд», «Оновлення».

Кожна картка — окремий розділ налаштувань, вони не знають одна про одну.
Живуть окремо від каркаса вікна саме тому, що змінюються найчастіше: додати
поле в «Чат» не повинно означати похід у код розміщення вікна.
"""

from typing import TYPE_CHECKING

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QFrame, QHBoxLayout, QLabel, QLineEdit, QPushButton,
    QSlider, QVBoxLayout, QWidget,
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

        # «Без рамки»: лише повідомлення, без бордюра й підкладки. Керування
        # (заголовок, кнопки) повертається, коли зняти замок — див. look._apply_chrome.
        self.frameless = QCheckBox("Без рамки (лише повідомлення)", self)
        self.frameless.setChecked(bool(getattr(self.win, "frameless", False)))
        self.frameless.setToolTip(
            "Прибирає рамку й підкладку — видно самі повідомлення. Коли замок "
            "увімкнено (миша провалюється крізь вікно), хром зникає повністю; щоб "
            "посунути чи налаштувати — зніміть замок (Ctrl+Alt+Space або кнопка "
            "замка), і заголовок з рамкою повернуться.")
        self.frameless.toggled.connect(self.win.set_frameless)
        lay.addWidget(self.frameless)

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
    def _collapsible(self, parent_lay, title: str):
        """Розкривна підсекція: кнопка-заголовок ▸/▾ показує/ховає вкладений блок.
        Повертає (layout_блоку, кнопка, блок) — контроли додавай у layout. Так
        ризиковані/рідкісні речі не лежать «під одним кліком»: людина розкриває
        їх свідомо."""
        btn = QPushButton("▸ " + title, self)
        btn.setObjectName("ghost")
        btn.setFixedHeight(28)
        btn.setStyleSheet("QPushButton { text-align:left; padding-left:8px; }")
        parent_lay.addWidget(btn)
        body = QWidget(self)
        blay = QVBoxLayout(body)
        blay.setContentsMargins(10, 2, 0, 6)
        blay.setSpacing(6)
        body.setVisible(False)

        def toggle(_=False):
            vis = not body.isVisible()
            body.setVisible(vis)
            btn.setText(("▾ " if vis else "▸ ") + title)
            # Вміст змінив висоту — переміряти й переставити панель у межах екрана.
            if hasattr(self, "cap_height"):
                self.cap_height()
                self.adjustSize()
                self.win._place_panel()

        btn.clicked.connect(toggle)
        parent_lay.addWidget(body)
        return blay, btn, body

    # --- секція «Чат поверх гри» ---------------------------------------------
    def _top_card(self) -> QFrame:
        """Чат поверх гри — по-простому.

        Один головний тумблер (безпечний спосіб). Стан гри — під ним. Рідше
        потрібне («не видно чат?») і ризиковане (інжект) — сховані в розкривні
        підсекції, щоб люди не вмикали все підряд.
        """
        card, lay = self._card("Чат поверх гри")

        # ГОЛОВНЕ: показати чат поверх гри (безпечний DComp-спосіб, без інжекту).
        self.dcomp_box = QCheckBox("Показувати чат поверх гри", self)
        self.dcomp_box.setToolTip(
            "Чат видно поверх будь-якої гри — навіть безрамкового повноекранного "
            "(як Hunt: Showdown), — і OBS його не знімає. У гру нічого не "
            "вкладається (безпечно для античитів). Позиція й розмір — як у цього "
            "вікна чату на робочому столі.")
        self.dcomp_box.toggled.connect(self.win.set_dcomp_overlay)
        lay.addWidget(self.dcomp_box)

        self.top_status = QLabel("", self)   # «Гра: <назва> — <режим>» (таймер)
        self.top_status.setObjectName("dim")
        self.top_status.setWordWrap(True)
        lay.addWidget(self.top_status)

        # =====================================================================
        # Підсекція «Не видно чат у грі?» — фолбеки: безрамковий режим і зняття
        # повноекранної оптимізації. Тут же вибір вікна гри для цих дій.
        fb, _, _ = self._collapsible(lay, "Не видно чат у грі?")

        fb.addWidget(self._label("Гра (для дій нижче):"))
        bpick_row = QHBoxLayout()
        bpick_row.setSpacing(6)
        self.border_pick = QComboBox(self)
        self.border_pick.setToolTip("Оберіть вікно гри для дій нижче.")
        self.border_pick.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.border_pick.setMinimumContentsLength(6)
        bpick_row.addWidget(self.border_pick, 1)
        self.border_refresh = QPushButton("⟳", self)
        self.border_refresh.setObjectName("ghost")
        self.border_refresh.setFixedSize(30, 28)
        self.border_refresh.setToolTip("Оновити список вікон.")
        self.border_refresh.clicked.connect(self._refresh_borderless)
        bpick_row.addWidget(self.border_refresh)
        fb.addLayout(bpick_row)

        self.fso_btn = QPushButton("Прибрати повноекранну оптимізацію гри", self)
        self.fso_btn.setObjectName("ghost")
        self.fso_btn.setFixedHeight(28)
        self.fso_btn.setToolTip(
            "Найнадійніше, коли чат видно в меню, а в бою зникає (Hunt): вимикає "
            "для гри «оптимізацію на весь екран». Діє з наступного запуску гри; у "
            "гру нічого не вкладається.")
        self.fso_btn.clicked.connect(self._toggle_fso)
        self.fso_btn.setEnabled(False)
        fb.addWidget(self.fso_btn)
        self.fso_hint = QLabel(
            "Після вмикання перезапустіть гру.", self)
        self.fso_hint.setObjectName("dim")
        self.fso_hint.setWordWrap(True)
        fb.addWidget(self.fso_hint)

        brow = QHBoxLayout()
        brow.setSpacing(6)
        self.borderless_btn = QPushButton("Зробити гру безрамковою", self)
        self.borderless_btn.setObjectName("ghost")
        self.borderless_btn.setFixedHeight(28)
        self.borderless_btn.setToolTip(
            "Знімає з вікна гри рамку і розтягує на монітор — тоді нею керує "
            "система і чат поверх видно. Нічого в гру не встановлюємо.")
        self.borderless_btn.clicked.connect(self._make_borderless)
        self.borderless_btn.setEnabled(False)
        brow.addWidget(self.borderless_btn, 1)
        self.restore_btn = QPushButton("Повернути", self)
        self.restore_btn.setObjectName("ghost")
        self.restore_btn.setFixedHeight(28)
        self.restore_btn.setToolTip("Повернути вікну гри те, що в нього було.")
        self.restore_btn.clicked.connect(self._restore_window)
        self.restore_btn.hide()
        brow.addWidget(self.restore_btn)
        fb.addLayout(brow)
        self.border_pick.currentIndexChanged.connect(self._sync_fso_button)

        # =====================================================================
        # Підсекція «Для досвідчених» — інжект (справжній чат усередині гри).
        # Найпотужніше й найризикованіше: вкладає бібліотеку в процес гри. Тому —
        # окремо, згорнуто й з попередженням. Для решти є спосіб вище.
        adv, self._adv_btn, self._adv_body = self._collapsible(lay, "Для досвідчених ⚠")

        self.game_box = QCheckBox("Справжній чат усередині гри (інжект)", self)
        self.game_box.setToolTip(
            "Вкладає бібліотеку в процес гри й малює чат усередині кадру — з "
            "аватарками й емоутами, навіть у виключному повноекранному. Потрібне "
            "рідко: спосіб вище працює майже скрізь. НЕ для онлайн-ігор з античитом.")
        self.game_box.toggled.connect(self._toggle_game)
        adv.addWidget(self.game_box)

        self.game_warn = QLabel(
            "Вкладення в процес гри. Бібліотека непідписана — Windows "
            "(SmartScreen/Defender) може перепитати. В ОНЛАЙН-іграх з античитом "
            "так робити НЕ можна (загроза бану) — для них є спосіб вище.", self)
        self.game_warn.setWordWrap(True)
        self.game_warn.setStyleSheet(
            "color:#fcd9a5; background:rgba(217,119,6,0.12);"
            "border:1px solid rgba(217,119,6,0.40); border-radius:6px; padding:6px 8px;")
        self.game_warn.hide()
        adv.addWidget(self.game_warn)

        self.game_help_btn = QPushButton("ℹ Що потрібно, щоб чат у грі запрацював", self)
        self.game_help_btn.setObjectName("ghost")
        self.game_help_btn.setFixedHeight(26)
        self.game_help_btn.clicked.connect(self._show_injector_help)
        adv.addWidget(self.game_help_btn)

        pick_row = QHBoxLayout()
        pick_row.setSpacing(6)
        self.game_pick = QComboBox(self)
        self.game_pick.setToolTip("Оберіть вікно гри, у яке вкласти чат.")
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
        adv.addLayout(pick_row)

        self.game_inject = QPushButton("Показати чат у грі", self)
        self.game_inject.setObjectName("ghost")
        self.game_inject.setFixedHeight(28)
        self.game_inject.setToolTip("Вкладе чат у вибрану гру.")
        self.game_inject.clicked.connect(self._inject_selected)
        adv.addWidget(self.game_inject)

        self.game_status = QLabel("", self)
        self.game_status.setObjectName("dim")
        self.game_status.setWordWrap(True)
        adv.addWidget(self.game_status)

        self.game_place_hint = QLabel(
            "📍 Де стоїть і як розтягнуте це вікно чату — там і такого ж розміру "
            "буде чат у грі. Пересунь/розтягни вікно чату — і чат у грі стане так само.", self)
        self.game_place_hint.setObjectName("dim")
        self.game_place_hint.setWordWrap(True)
        adv.addWidget(self.game_place_hint)

        op_row = QHBoxLayout()
        op_row.setSpacing(6)
        self.game_op_lbl = QLabel("Прозорість:", self)
        op_row.addWidget(self.game_op_lbl)
        self.game_opacity = QSlider(Qt.Horizontal, self)
        self.game_opacity.setRange(30, 255)
        self.game_opacity.setValue(int(self.win.game_opacity))
        self.game_opacity.valueChanged.connect(lambda v: self.win.set_game_opacity(v))
        op_row.addWidget(self.game_opacity, 1)
        adv.addLayout(op_row)

        self.game_hide_obs = QCheckBox("Ховати чат від OBS (видно лише мені)", self)
        self.game_hide_obs.setChecked(bool(self.win.game_hide_obs))
        self.game_hide_obs.toggled.connect(self.win.set_game_hide_obs)
        adv.addWidget(self.game_hide_obs)
        self.game_hide_obs_hint = QLabel(
            "Працює на всіх API (DirectX 9/11/12, OpenGL, Vulkan): у грі видно, в "
            "ефір не потрапляє.", self)
        self.game_hide_obs_hint.setObjectName("dim")
        self.game_hide_obs_hint.setWordWrap(True)
        adv.addWidget(self.game_hide_obs_hint)

        # Внутрішні контроли інжектора показуються, лише коли увімкнено game_box.
        self._game_widgets = (self.game_warn, self.game_help_btn,
                              self.game_pick, self.game_refresh,
                              self.game_inject, self.game_status,
                              self.game_place_hint,
                              self.game_op_lbl, self.game_opacity,
                              self.game_hide_obs, self.game_hide_obs_hint)
        for w in self._game_widgets:
            w.hide()
        self._injected = set()   # hwnd, куди вже вкладено — щоб не інжектити двічі
        self.set_game_experimental(True)   # доступно в усіх каналах (за наявності файлів)

        # =====================================================================
        self.keep_top = QCheckBox("Тримати вікно чату поверх усіх вікон", self)
        self.keep_top.setToolTip(
            "У рідкісних старих іграх це дає мерехтіння — тоді вимкніть.")
        self.keep_top.toggled.connect(self.win.set_keep_top)
        lay.addWidget(self.keep_top)

        self._refresh_borderless()     # наповнити список вікон одразу
        return card

    # --- інжектор чату в гру ---
    def set_game_experimental(self, on: bool):
        """Показує підсекцію інжектора «Для досвідчених», коли поруч є нативні
        файли. Немає файлів — ховаємо всю підсекцію (і заголовок, і тіло), щоб не
        світити порожнім розділом."""
        show = on and inject_mod.available()
        if hasattr(self, "_adv_btn"):
            self._adv_btn.setVisible(show)
        self.game_box.setVisible(show)
        if not show:
            if hasattr(self, "_adv_body"):
                self._adv_body.setVisible(False)
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
        self._sync_fso_button()

    def _sync_fso_button(self):
        """Вмикає кнопку FSO для вибраної гри й підписує її під поточний стан
        (прибрати / повернути оптимізацію)."""
        hwnd = self.border_pick.currentData()
        if not hwnd:
            self.fso_btn.setEnabled(False)
            self.fso_btn.setText("Прибрати повноекранну оптимізацію гри")
            return
        self.fso_btn.setEnabled(True)
        path = fs_mod.game_exe_path(int(hwnd))
        off = fs_mod.fullscreen_opt_disabled(path) if path else False
        self.fso_btn.setText("Повернути повноекранну оптимізацію гри" if off
                             else "Прибрати повноекранну оптимізацію гри")

    def _toggle_fso(self):
        hwnd = self.border_pick.currentData()
        if not hwnd:
            self.top_status.setText("Спершу оберіть вікно гри зі списку (⟳ оновлює).")
            return
        res = self.win.toggle_fullscreen_opt(int(hwnd))
        if res is None:
            self.top_status.setText("Не вдалося змінити налаштування гри "
                                    "(не знайшов .exe або немає доступу).")
            return
        if res:
            self.top_status.setText("Готово: повноекранну оптимізацію вимкнено. "
                                    "Перезапустіть гру — і чат буде видно в бою.")
        else:
            self.top_status.setText("Повноекранну оптимізацію повернено як було.")
        self._sync_fso_button()

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
