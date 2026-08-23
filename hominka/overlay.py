"""Вікно чату — головне вікно програми.

Тут лишилося саме вікно: рамка без системного заголовка, геометрія, прозорість,
кегль, клік-крізь, гаряча клавіша і показ сторінки. Усе інше живе поруч і
підмішується сюди:

    sources.py    звідки береться чат (площадки, стрічка, проба YouTube)
    updating.py   перевірка, завантаження і встановлення оновлення
    config.py     читання і запис config.json

Розділено саме так, бо ці три теми змінюються незалежно: додати площадку — це
sources, поміняти канал оновлень — updating, а рамку вікна не чіпає ні те, ні
інше.
"""

import os
import sys
from ctypes import wintypes

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QIcon
from PySide6.QtWidgets import QApplication, QFrame, QMainWindow, QVBoxLayout
from PySide6.QtWebEngineCore import QWebEnginePage
from PySide6.QtWebEngineWidgets import QWebEngineView

from . import feed as chatfeed
from . import update as updater
from .config import ConfigMixin
from . import fullscreen, rtss, x11
from .paths import BASE_DIR, resource_path
from .probe import LiveProbe
from .sources import SourcesMixin
from .splash import close_splash, splash_text
from .look import LookMixin
from .styles import ACCENT_ACTIVE
from .ui.banner import UpdateBanner
from .ui.chrome import DragBar, SizeGrip
from .ui.panel import SettingsPanel
from .updating import UpdatingMixin
from .urls import is_youtube, resolve_chat_url
from .version import APP_ICON, APP_NAME, APP_VERSION
from .webprofile import build_profile
from .winapi import (
    HOTKEY_ID, IS_WINDOWS, MOD_ALT, MOD_CONTROL, MOD_NOREPEAT, VK_SPACE, WM_HOTKEY,
    _hwnd, exclude_from_capture, hide_new_windows_from_capture, user32,
)
from .yt import YT_ALL_MESSAGES_JS, YT_MENTIONS_JS, YT_REACTIONS_JS, YT_STYLE_JS


class Overlay(SourcesMixin, UpdatingMixin, ConfigMixin, LookMixin, QMainWindow):


    def __init__(self, url: str | None = None):
        super().__init__()
        self._reset_state(url)
        self._build_window()
        self._build_web()
        self._restore_and_open()
        self._start_timers()

    # --- складання вікна ------------------------------------------------------
    #
    # Розбито на чотири кроки не заради краси: у конструкторі на сто тридцять
    # рядків не видно, що від чого залежить, і додати щось на потрібне місце
    # можна лише вгадуючи. Порядок кроків значущий — стан, вікно, сторінка,
    # відновлені налаштування, і лише потім таймери, яким усе це вже потрібне.

    def _reset_state(self, url: str | None):
        """Поля, з якими вікно народжується. Мережі й віджетів тут ще немає."""
        self.click_through = False
        self.zoom = 1.0            # масштаб тексту чату
        self.bg_alpha = 0.30       # затемнення підкладки під чатом
        self.accent = ACCENT_ACTIVE
        self.mode = "web"          # web = сторінка чату, feed = спільна стрічка
        self._cli_url = url        # URL з аргументу командного рядка (пріоритет)
        # Чат сайту: посилання з ⚙ (config.json). Порожнє, поки не вписали —
        # тоді вікно чесно каже, чого йому бракує, замість порожньої сторінки.
        self.site_url = ""
        # Свій CSS для чату (редактор — cssui/). Порожній = типове оформлення;
        # воно й є те, що людина бачить, поки нічого не змінювала.
        self.custom_css = ""
        # Порядок частин рядка чату. Типовий — той, що був завжди.
        self.chat_layout = list(chatfeed.DEFAULT_LAYOUT)
        self.css_window = None
        # Слід від щойно встановленого оновлення (пишеться перед перезапуском).
        self.updated_to = ""
        self.updated_notes = ""
        self.url = resolve_chat_url(url) if url else ""
        self.is_yt = is_youtube(self.url)

        # Власна трансляція (див. probe.py) і канал, заданий руками.
        self.my_channel = ""
        # Додаткові площадки: коли задано хоч одну, чат збирається в спільну
        # стрічку замість сторінки YouTube.
        self.twitch_channel = ""
        self.kick_channel = ""
        self.readers = []
        self.feed = None
        # Затримка стрічки, секунди. Стосується спільної стрічки: сторінкою
        # YouTube ми не керуємо, там повідомлення малює він сам.
        self.chat_delay = 0
        self.auto_video = ""
        self.yt_channel_id = ""
        self.yt_channel_title = ""

        # Оновлення (див. updating.py).
        self.channel = updater.DEFAULT_CHANNEL
        self.installed_channel = ""   # з якого каналу стоїть поточна збірка
        self.auto_update = True
        self.pending = None           # Release, який чекає на згоду користувача
        self.downloaded = ""          # шлях до завантаженого архіву
        self._updated_banner = None

        # Запис налаштувань із дебаунсом — щоб не писати на диск на кожен
        # піксель під час зміни розміру (звідси мікрофрізи). Заводимо ЩЕ ДО
        # читання конфігу: перші ж налаштування, які його торкнуться, уже
        # просять зберегтися.
        self._save_timer = QTimer(self)
        self._save_timer.setSingleShot(True)
        self._save_timer.setInterval(400)
        self._save_timer.timeout.connect(self._write_config)

        # Утримання вікна зверху (див. fullscreen.py). Вмикається галочкою в ⚙:
        # у рідкісних старих іграх воно дає мерехтіння, і людина мусить мати
        # змогу його вимкнути.
        self.keep_top = True
        self._topmost = fullscreen.TopMostKeeper(self)

        # Дзеркало чату в OSD RTSS — єдиний спосіб побачити чат у виключному
        # повноекранному режимі, не лізучи в чужий процес (див. rtss.py).
        self.rtss_on = False
        self.rtss = rtss.ChatMirror()

        # Справжній чат у грі через інжектор (native/). Створюємо лениво —
        # тільки коли вмикають, бо це друге приховане вікно з рушієм браузера.
        self.game_on = False
        self.game_overlay = None
        self.game_anchor = "tl"
        self.game_margin_x = 24
        self.game_margin_y = 24
        self.game_opacity = 235

    def _build_window(self):
        """Рамка без системного заголовка: смужка, смужка оновлення, куточок."""
        self.setWindowFlags(
            Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
        )
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setWindowTitle(APP_NAME)
        self.setWindowIcon(QIcon(resource_path(APP_ICON)))

        self.frame = QFrame(self)
        self.frame.setObjectName("frame")
        self._apply_border(ACCENT_ACTIVE)

        self._vbox = QVBoxLayout(self.frame)
        self._vbox.setContentsMargins(3, 3, 3, 3)
        self._vbox.setSpacing(0)

        self.bar = DragBar(self)
        self._vbox.addWidget(self.bar)

        self.banner = UpdateBanner(self)
        self._vbox.addWidget(self.banner)

    def _build_web(self):
        """Сторінка чату і все, що з нею пов'язано."""
        # Профіль тримаємо в полі: сторінка живе лише поки живий профіль, і без
        # посилання Qt зносить його разом із входом у YouTube.
        self.profile = build_profile(self)
        self.view = QWebEngineView(self)
        self.view.setPage(QWebEnginePage(self.profile, self.view))
        self.view.page().setBackgroundColor(QColor(0, 0, 0, 0))
        self.view.setAttribute(Qt.WA_TranslucentBackground, True)
        self.view.loadFinished.connect(self._on_loaded)
        self._vbox.addWidget(self.view, 1)
        self.feed = chatfeed.ChatFeed(self.view)

        self.setCentralWidget(self.frame)
        self.panel = SettingsPanel(self)
        self.grip = SizeGrip(self.frame, ACCENT_ACTIVE)

    def _restore_and_open(self):
        """Налаштування з диска — і одразу те джерело, яке з них випливає."""
        self._load_config()
        if self._updated_banner:
            self.banner.show_updated(*self._updated_banner)
        if not self._cli_url and (self.twitch_channel or self.kick_channel):
            self._start_feed()
        else:
            self._show_url()

    def _start_timers(self):
        """Усі повторювані справи вікна — в одному місці, з поясненням кожної.

        Їх три: пошук власного ефіру, перевірка оновлень і підстраховка
        захисту від захоплення екрана. Четвертий — запис налаштувань — живе в
        _reset_state, бо потрібен раніше за все інше.
        """
        # Пошук власної трансляції: окрема прихована сторінка в тому ж профілі.
        self.probe = LiveProbe(self.profile, self)
        self.probe.result.connect(self._on_probe)
        self._probe_timer = QTimer(self)
        self._probe_timer.setInterval(self._probe_interval())
        self._probe_timer.timeout.connect(self.probe_live)
        self.panel.set_source_status(self)
        # Перша проба — після того, як вікно вже показало чат: спершу картинка,
        # потім мережа.
        QTimer.singleShot(4000, self.probe_live)
        self._probe_timer.start()

        # Оновлювач. Перша перевірка — із затримкою: старт програми і так
        # завантажує сторінку чату, лізти в мережу одночасно ні до чого.
        self.updater = updater.Updater(APP_VERSION, os.path.dirname(sys.executable)
                                       if getattr(sys, "frozen", False) else BASE_DIR, self)
        self.updater.checked.connect(self._on_checked)
        self.updater.progress.connect(self._on_progress)
        self.updater.downloaded.connect(self._on_downloaded)
        self.updater.failed.connect(self._on_update_failed)
        self._upd_timer = QTimer(self)
        self._upd_timer.setInterval(6 * 60 * 60 * 1000)   # раз на 6 годин
        self._upd_timer.timeout.connect(lambda: self.check_updates(manual=False))
        if self.auto_update:
            QTimer.singleShot(15000, lambda: self.check_updates(manual=False))
            self._upd_timer.start()
        # Хвости від минулих разів: перерване завантаження або оновлення, яке
        # так і не поставили.
        QTimer.singleShot(3000, updater.cleanup_downloads)

        # Підстраховка до CaptureGuard: якщо якесь вікно з'явиться повз подію
        # Show (або Qt перестворить його), обхід усе одно його прикриє.
        self._capture_timer = QTimer(self)
        self._capture_timer.setInterval(1000)
        self._capture_timer.timeout.connect(hide_new_windows_from_capture)
        self._capture_timer.timeout.connect(self._topmost.tick)
        self._capture_timer.timeout.connect(self._refresh_fullscreen_state)
        self._capture_timer.start()


    def set_custom_css(self, css: str):
        """Свій CSS — у вікно чату негайно і в config.json.

        Негайно — принципово: стилі підбирають, дивлячись на живий чат, а не
        перезапускаючи програму після кожної правки.
        """
        self.custom_css = css or ""
        self.save_config()
        if self.feed is not None:
            self.feed.set_custom_css(self.custom_css)
        if self.game_overlay is not None:
            self.game_overlay.set_custom_css(self.custom_css)
        if self.mode == "web":
            self._inject_custom_css()

    def set_chat_layout(self, layout):
        """Порядок частин рядка — у вікно чату негайно і в config.json.

        Стосується лише спільної стрічки: сторінку YouTube чи чат сайту малюємо
        не ми, і переставляти там нічого.
        """
        self.chat_layout = chatfeed.clean_layout(layout)
        self.save_config()
        if self.feed is not None:
            self.feed.set_layout(self.chat_layout)
        if self.game_overlay is not None:
            self.game_overlay.set_layout(self.chat_layout)

    def _inject_custom_css(self):
        """Кладе свій CSS і на звичайну сторінку чату (сайт або YouTube).

        Селектори там чужі, але людина, яка полізла в CSS, з чужими розбереться
        — а от «мій CSS працює тільки в одному з трьох режимів» пояснити було б
        нічим.
        """
        if not self.custom_css:
            return
        self.view.page().runJavaScript(chatfeed.apply_css_js(self.custom_css))

    def open_css_editor(self):
        """Відкриває редактор CSS (вікно створюється раз і живе далі)."""
        # Імпорт саме тут: вікно редактора тягне за собою ще одну сторінку
        # браузера, а відкривають його одиниці — платити за це секундою старту
        # кожного запуску ні до чого.
        from . import cssui as css_editor
        if self.css_window is None:
            self.css_window = css_editor.CssEditor(self)
        self.css_window.show()
        self.css_window.raise_()
        self.css_window.activateWindow()

    def _on_loaded(self, ok: bool):
        self.view.setZoomFactor(self.zoom)
        if ok and self.mode == "feed":
            self.feed.on_loaded()
            return
        if ok:
            self._inject_custom_css()
        if ok and self.is_yt:
            # гарний прозорий стиль поверх YouTube-чату
            self.view.page().runJavaScript(YT_STYLE_JS)
            # ...показуємо ВСІ повідомлення, а не «цікаві»...
            self.view.page().runJavaScript(YT_ALL_MESSAGES_JS)
            # ...підсвічуємо «@нік», щоб було видно, кому відповідають...
            self.view.page().runJavaScript(YT_MENTIONS_JS)
            # ...і витягуємо панель реакцій із прихованої смуги вводу
            self.view.page().runJavaScript(YT_REACTIONS_JS)

    # --- налаштування панель (окреме верхнє вікно) ---
    def toggle_settings(self):
        if self.panel.isVisible():
            self.panel.hide()
        else:
            self.panel.adjustSize()
            self._place_panel()
            self.panel.show()
            self.panel.raise_()
            self.panel.activateWindow()

    def _place_panel(self):
        """Ставить вікно налаштувань ПОРУЧ із чатом, а не поверх нього.

        Праворуч, якщо там є місце; інакше ліворуч; якщо тісно з обох боків —
        притискаємо до краю екрана. Чат при цьому лишається видимим: його ж і
        налаштовують.

        Тінь навколо картки — частина вікна, тому в розрахунках її знімаємо,
        інакше між чатом і панеллю зяяла б порожня смуга.
        """
        pad = self.panel.SHADOW
        w, h = self.panel.width(), self.panel.sizeHint().height()
        scr = self.screen().availableGeometry() if self.screen() else None

        right = self.x() + self.width() + self.panel.GAP - pad
        left = self.x() - w + pad - self.panel.GAP
        x = right
        if scr and right + w - pad > scr.right():
            x = left if left + pad >= scr.left() else right

        y = self.y() - pad + 2
        if scr:
            x = max(scr.left() - pad, min(x, scr.right() - w + pad))
            y = max(scr.top() - pad, min(y, scr.bottom() - h + pad))
        self.panel.move(x, y)

    def moveEvent(self, e):
        # Вікно тягнуть — панель їде разом, інакше вона лишається «висіти»
        # посеред екрана окремо від чату.
        super().moveEvent(e)
        if hasattr(self, "panel") and self.panel.isVisible():
            self._place_panel()


    def resizeEvent(self, e):
        super().resizeEvent(e)
        self.grip.move(self.width() - self.grip.width() - 3,
                       self.height() - self.grip.height() - 3)
        self.grip.raise_()
        if self.panel.isVisible():
            self._place_panel()
        self.save_config()

    def showEvent(self, e):
        super().showEvent(e)
        if IS_WINDOWS and not exclude_from_capture(self):
            print("[chat-overlay] УВАГА: не вдалося виключити з захоплення "
                  "(потрібна Windows 10 2004+/11). OBS може бачити вікно.")
        self._register_hotkey()
        # Linux: підказки композитору (див. x11.py). У Windows нічого не робить.
        x11.apply_overlay_hints(self)
        # Заставка збірки одним файлом: доводимо смугу до кінця і знімаємо —
        # вікно вже на екрані, і тягнути далі нема чого.
        splash_text("Готово", 100)
        close_splash()


    def _register_hotkey(self):
        """Ctrl+Alt+Space з будь-якого вікна.

        Системного гарячого клавіша поза фокусом у Linux немає (це справа
        менеджера вікон, а не програми), тому там лишається кнопка 🔓 на
        панелі — і клік-крізь з неї вмикається так само.
        """
        if not IS_WINDOWS:
            return
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

    def _refresh_fullscreen_state(self):
        """Питає систему, що зараз попереду, і показує це в ⚙.

        Раз на секунду і тільки коли панель відкрита: інакше це опитування
        нікому не потрібне — людина його все одно не бачить.
        """
        if not self.panel.isVisible():
            return
        self.panel.set_fullscreen_state(fullscreen.state())

    # --- справжній чат у грі (інжектор) ---
    def _ensure_game_overlay(self):
        if self.game_overlay is None:
            from .gameoverlay import GameOverlay
            self.game_overlay = GameOverlay(self)
            self.game_overlay.set_geometry(self.game_anchor, self.game_margin_x,
                                           self.game_margin_y, self.game_opacity)
        return self.game_overlay

    def set_game_overlay(self, on: bool):
        """Вмикає/вимикає продюсера кадру чату для гри.

        У стабільному каналі — ніколи (як і RTSS): експеримент з інʼєкцією не
        має вмикатися в тих, хто просто веде ефір.
        """
        if on and self.channel == "stable":
            on = False
        self.game_on = bool(on)
        if self.game_on:
            self._ensure_game_overlay().set_enabled(True)
        elif self.game_overlay is not None:
            self.game_overlay.set_enabled(False)
        self.save_config()

    def inject_game(self, hwnd: int):
        """Кладе overlay.dll у вікно hwnd і, якщо вдалося, вмикає продюсера."""
        from . import inject
        res = inject.inject(hwnd)
        if res.ok:
            self.set_game_overlay(True)
        return res

    def set_game_geometry(self, anchor: str, margin_x: int, margin_y: int, opacity: int):
        self.game_anchor = anchor
        self.game_margin_x = int(margin_x)
        self.game_margin_y = int(margin_y)
        self.game_opacity = int(opacity)
        if self.game_overlay is not None:
            self.game_overlay.set_geometry(anchor, margin_x, margin_y, opacity)
        self.save_config()

    def set_rtss(self, on: bool):
        """Вмикає дублювання чату в OSD RTSS.

        У стабільному каналі — ніколи. Це остання застава: галочки там і так
        не видно, але жоден config.json — свій, чужий чи принесений з бети —
        не має права оживити в стабільній програмі те, чого в ній немає.
        """
        if on and self.channel == "stable":
            on = False
        self.rtss_on = bool(on) and self.rtss.set_enabled(bool(on))
        if not on:
            self.rtss.set_enabled(False)
        self.save_config()
        return self.rtss_on

    def set_keep_top(self, on: bool):
        self.keep_top = bool(on)
        self._topmost.enabled = self.keep_top
        self.save_config()

    def make_game_borderless(self):
        """Переводить вікно гри в безрамковий режим — на прохання людини."""
        info = fullscreen.state()
        if info.get("kind") in ("none", "desktop"):
            return False
        return fullscreen.make_borderless(info.get("hwnd", 0))

    def restore_game_window(self):
        return fullscreen.restore(fullscreen.changed_window())

    def closeEvent(self, e):
        # Слот OSD чужий, і лишати в ньому свій текст після виходу не можна:
        # RTSS показуватиме його доти, доки сам не перезапуститься.
        self.rtss.stop()
        if self.game_overlay is not None:
            self.game_overlay.close()
        self._stop_readers()
        # Завантажене, але не встановлене оновлення — 220 МБ у тимчасовій теці.
        # Якщо людина закриває програму, не поставивши його, тримати файл
        # немає сенсу: наступного разу він завантажиться заново.
        if self.downloaded:
            updater.cleanup_downloads()
        self._write_config()
        if IS_WINDOWS:
            try:
                user32.UnregisterHotKey(_hwnd(self), HOTKEY_ID)
            except Exception:
                pass
        # Явне прибирання — інакше вікно налаштувань (окреме верхнє вікно) та
        # дочірній QtWebEngineProcess лишають процес висіти після закриття ✕.
        try:
            self.panel.close()
            self.panel.deleteLater()
        except Exception:
            pass
        try:
            self.view.stop()
            self.view.setParent(None)
            self.view.deleteLater()
        except Exception:
            pass
        super().closeEvent(e)
        QApplication.quit()
