"""Вікно чату — головне вікно програми.

Тут зібрано те, що стосується САМОГО вікна: звідки воно бере чат, як зберігає
налаштування, як поводиться під час оновлення. Оформлення живе в styles.py,
Windows-трюки — у winapi.py, читачі площадок — окремими модулями.
"""

import json
import os
import sys

from PySide6.QtCore import Qt, QTimer, QUrl
from PySide6.QtGui import QColor, QIcon
from PySide6.QtWidgets import QApplication, QFrame, QMainWindow, QVBoxLayout
from PySide6.QtWebEngineCore import QWebEnginePage
from PySide6.QtWebEngineWidgets import QWebEngineView
from ctypes import wintypes

from . import chat_kick, chat_twitch, chat_youtube, chatfeed, chatsources as cs, updater
from .notes import update_status_html
from .paths import BASE_DIR, CONFIG_PATH, resource_path
from .probe import LiveProbe
from .splash import close_splash
from .styles import ACCENT_ACTIVE, ACCENT_LOCKED, NO_SOURCE_HTML
from .ui.banner import UpdateBanner
from .ui.chrome import DragBar, SizeGrip
from .ui.panel import SettingsPanel
from .ytinject import (
    YT_ALL_MESSAGES_JS, YT_MENTIONS_JS, YT_REACTIONS_JS, YT_STYLE_JS,
)
from .urls import channel_id_from, is_youtube, resolve_chat_url, site_chat_url
from .version import APP_ICON, APP_NAME, APP_VERSION
from .webprofile import build_profile
from .winapi import (
    HOTKEY_ID, IS_WINDOWS, MOD_ALT, MOD_CONTROL, MOD_NOREPEAT, VK_SPACE, WM_HOTKEY,
    _hwnd, exclude_from_capture, hide_new_windows_from_capture, set_click_through,
    user32,
)

class Overlay(QMainWindow):
    def __init__(self, url: str | None = None):
        super().__init__()
        self.click_through = False
        self.zoom = 1.0            # масштаб тексту чату
        self.bg_alpha = 0.30       # затемнення підкладки під чатом
        self.accent = ACCENT_ACTIVE
        self.mode = "web"          # web = сторінка чату, feed = спільна стрічка
        self._cli_url = url        # URL з аргументу командного рядка (пріоритет)
        # Чат сайту: посилання з ⚙ (config.json). Порожнє, поки не вписали —
        # тоді вікно чесно каже, чого йому бракує, замість порожньої сторінки.
        self.site_url = ""
        # Свій CSS для чату (редактор — css_editor.py). Порожній = типове
        # оформлення; воно й є те, що людина бачить, поки нічого не змінювала.
        self.custom_css = ""
        self.css_window = None
        # Слід від щойно встановленого оновлення (пишеться перед перезапуском).
        self.updated_to = ""
        self.updated_notes = ""
        self.url = resolve_chat_url(url) if url else ""
        self.is_yt = is_youtube(self.url)

        # Власна трансляція (див. LiveProbe) і канал, заданий руками.
        self.my_channel = ""
        # Додаткові площадки: коли задано хоч одну, чат збирається в спільну
        # стрічку (chatfeed) замість сторінки YouTube.
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

        # Оновлення (див. updater.py).
        self.channel = updater.DEFAULT_CHANNEL
        self.installed_channel = ""   # з якого каналу стоїть поточна збірка
        self.auto_update = True
        self.pending = None           # Release, який чекає на згоду користувача
        self.downloaded = ""          # шлях до завантаженого архіву

        # Збереження config з дебаунсом — щоб не писати на диск на кожен піксель
        # під час зміни розміру (звідси мікрофрізи).
        self._save_timer = QTimer(self)
        self._save_timer.setSingleShot(True)
        self._save_timer.setInterval(400)
        self._save_timer.timeout.connect(self._write_config)

        self.setWindowFlags(
            Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
        )
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setWindowTitle(APP_NAME)
        self.setWindowIcon(QIcon(resource_path(APP_ICON)))

        self.frame = QFrame(self)
        self.frame.setObjectName("frame")
        self._apply_border(ACCENT_ACTIVE)

        vbox = QVBoxLayout(self.frame)
        vbox.setContentsMargins(3, 3, 3, 3)
        vbox.setSpacing(0)

        self.bar = DragBar(self)
        vbox.addWidget(self.bar)

        self.banner = UpdateBanner(self)
        vbox.addWidget(self.banner)

        # Профіль тримаємо в полі: сторінка живе лише поки живий профіль, і без
        # посилання Qt зносить його разом із входом у YouTube.
        self.profile = build_profile(self)
        self.view = QWebEngineView(self)
        self.view.setPage(QWebEnginePage(self.profile, self.view))
        self.view.page().setBackgroundColor(QColor(0, 0, 0, 0))
        self.view.setAttribute(Qt.WA_TranslucentBackground, True)
        self.view.loadFinished.connect(self._on_loaded)
        vbox.addWidget(self.view, 1)
        self.feed = chatfeed.ChatFeed(self.view)

        self.setCentralWidget(self.frame)

        self.panel = SettingsPanel(self)

        self.grip = SizeGrip(self.frame, ACCENT_ACTIVE)

        self._updated_banner = None
        self._load_config()
        if self._updated_banner:
            self.banner.show_updated(*self._updated_banner)
        if not self._cli_url and (self.twitch_channel or self.kick_channel):
            self._start_feed()
        else:
            self._show_url()

        # Пошук власної трансляції: окрема прихована сторінка в тому ж профілі.
        self.probe = LiveProbe(self.profile, self)
        self.probe.result.connect(self._on_probe)
        self._probe_timer = QTimer(self)
        self._probe_timer.setInterval(self._probe_interval())
        self._probe_timer.timeout.connect(self.probe_live)
        self.panel.set_source_status(self)
        # Перша проба — після того, як вікно вже показало чат сайту: спершу
        # картинка, потім мережа.
        QTimer.singleShot(4000, self.probe_live)
        self._probe_timer.start()

        # Оновлювач. Перша перевірка — з затримкою: старт програми і так
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
        self._capture_timer.start()

    def _apply_border(self, accent: str):
        self.accent = accent
        self.frame.setStyleSheet(
            "#frame {"
            f" background: rgba(12,12,15,{self.bg_alpha:.2f});"
            f" border: 2px solid {accent};"
            " border-radius: 11px;"
            " }"
        )
        if hasattr(self, "grip"):
            self.grip.set_accent(accent)

    # --- оновлення ---
    def set_channel(self, channel: str):
        """Зміна каналу оновлень. Перевіряємо одразу: людина щойно попросила
        іншу гілку — вона й чекає результату, а не наступної перевірки за 6 годин."""
        if not channel or channel == self.channel:
            return
        self.channel = channel
        self.save_config()
        self.check_updates(manual=True)

    def check_updates(self, manual: bool = False):
        if getattr(self, "updater", None) is None:
            return
        if not getattr(sys, "frozen", False):
            # Запущено з коду — підміняти теку нічим і нема чого.
            if manual:
                self.panel.set_status("Запущено з коду — оновлення не застосовуються.")
            return
        if self.updater.busy:
            return
        self._manual_check = manual
        self.panel.set_status("Перевіряю оновлення…")
        self.updater.check(self.channel, self.installed_channel)

    def _on_checked(self, rel, err: str):
        if err:
            self.panel.set_status("Не вдалося перевірити: " + err)
            return
        if rel is None:
            self.pending = None
            self.panel.set_status("Версія %s — актуальна (%s)."
                                  % (APP_VERSION, updater.channel_label(self.channel)))
            return
        self.pending = rel
        self.panel.set_status(update_status_html(rel))
        self.banner.show_release(rel)
        # Панель налаштувань — окреме вікно поверх; поки вона відкрита, смужку
        # з оновленням видно погано. Ховаємо: рішення тепер приймають у ній.
        if self.panel.isVisible():
            self.panel.hide()
        # Обов'язкове оновлення — це виправлення, без якого програма працює
        # неправильно. Не ставимо мовчки, але й не даємо про нього забути.
        if rel.mandatory and self.auto_update:
            self.start_update()

    def start_update(self):
        if self.pending is None:
            self.check_updates(manual=True)
            return
        # Далі програма перезапуститься — тримати відкриту панель ні до чого.
        self.panel.hide()
        self.banner.show_progress(0, self.pending.size)
        self.updater.download(self.pending)

    def _on_progress(self, done: int, total: int):
        self.banner.show_progress(done, total)

    def on_update_button(self):
        """Одна кнопка на два кроки: спершу завантажити, потім встановити."""
        if self.downloaded:
            self.install_update()
        else:
            self.start_update()

    def _on_downloaded(self, path: str):
        self.downloaded = path
        self.banner.show_ready(self.pending) if self.pending else None
        self.panel.set_status("Завантажено %s — натисніть «Встановити»."
                              % (self.pending.version if self.pending else ""))

    def install_update(self):
        """Ставить завантажене й виходить: підмінник чекає саме виходу.

        Спершу показуємо, що відбувається, і лише потім закриваємось: інакше
        натискання «Встановити» виглядає як вилітання програми.
        """
        if not self.downloaded:
            return
        self.banner.show_installing(self.pending.version if self.pending else "")
        QTimer.singleShot(1200, self._do_install)

    def _do_install(self):
        if not self.downloaded:
            return
        try:
            app_dir = os.path.dirname(sys.executable) if getattr(sys, "frozen", False) else BASE_DIR
            # Канал запам'ятовуємо ДО перезапуску: після нього це вже інша збірка,
            # і без запису вона не знала б, з якої гілки прийшла.
            self.installed_channel = self.channel
            # Позначка «ми щойно оновлювались»: після перезапуску за нею
            # програма скаже, що саме сталося, — інакше вона просто зникає й
            # з'являється, і зрозуміти це неможливо.
            self.updated_to = self.pending.version if self.pending else ""
            self.updated_notes = self.pending.notes if self.pending else ""
            self._write_config()
            updater.install(self.downloaded, app_dir)
        except Exception as e:
            self.banner.show_error(str(e))
            return
        self.downloaded = ""
        self.close()

    def _on_update_failed(self, msg: str):
        self.banner.show_error(msg)
        self.panel.set_status("Оновлення не вдалося: " + msg)

    # --- вхід у YouTube ---
    def set_my_channel(self, text: str):
        """Канал, заданий руками. Скидаємо знайдений id: вписали інший канал —
        шукати треба заново."""
        text = (text or "").strip()
        if text == self.my_channel:
            return
        self.my_channel = text
        self.yt_channel_id = channel_id_from(text)
        self.probe.channel_id = self.yt_channel_id
        self.probe.channel_title = ""
        self.auto_video = ""
        self.save_config()
        self.panel.set_source_status(self)
        self.probe_live()

    def set_site_url(self, text: str):
        """Посилання на чат сайту з ⚙. Порожнє — теж відповідь: чат сайту не
        показуємо взагалі, залишаються площадки."""
        text = (text or "").strip()
        if text == self.site_url:
            return
        self.site_url = text
        self.save_config()
        self.panel.set_source_status(self)
        self.refresh_source()

    def set_custom_css(self, css: str):
        """Свій CSS — у вікно чату негайно і в config.json.

        Негайно — принципово: стилі підбирають, дивлячись на живий чат, а не
        перезапускаючи програму після кожної правки.
        """
        self.custom_css = css or ""
        self.save_config()
        if self.feed is not None:
            self.feed.set_custom_css(self.custom_css)
        if self.mode == "web":
            self._inject_custom_css()

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
        from . import css_editor
        if self.css_window is None:
            self.css_window = css_editor.CssEditor(self)
        self.css_window.show()
        self.css_window.raise_()
        self.css_window.activateWindow()

    def set_chat_delay(self, seconds: int):
        self.chat_delay = max(0, int(seconds))
        if self.feed is not None:
            self.feed.set_delay(self.chat_delay)
        self.save_config()

    def set_extra_channels(self, twitch: str, kick: str):
        """Канали Twitch і Kick із налаштувань. Приймаємо і посилання, і нік."""
        tw_platform, tw = cs.parse_source(twitch)
        kk_platform, kk = cs.parse_source(kick)
        # Голий нік площадка не вгадає — тоді беремо як є, з відповідного поля.
        if not tw and twitch.strip():
            tw = twitch.strip().lower().lstrip("@")
        if not kk and kick.strip():
            kk = kick.strip().lower().lstrip("@")
        if tw_platform and tw_platform != cs.TWITCH:
            tw = ""
        if kk_platform and kk_platform != cs.KICK:
            kk = ""
        if tw == self.twitch_channel and kk == self.kick_channel:
            return
        self.twitch_channel, self.kick_channel = tw, kk
        self.save_config()
        self.panel.set_source_status(self)
        self.refresh_source()

    # --- пошук власної трансляції ---
    def probe_live(self):
        """Питає YouTube, чи йде зараз ефір на нашому каналі."""
        if self.probe.busy:
            return
        self.probe.start(self.my_channel, self.yt_channel_id)

    def _on_probe(self, info: dict):
        self.yt_channel_id = info.get("channelId") or ""
        self.yt_channel_title = info.get("title") or ""
        video = info.get("video") or ""
        changed = video != self.auto_video
        self.auto_video = video
        self.save_config()
        self.panel.set_source_status(self)
        # У режимі спільної стрічки трансляцію шукає сам читач YouTube, і
        # перезбирати стрічку через нього не можна: це стерло б уже показані
        # повідомлення Twitch і Kick.
        if changed and self.mode != "feed":
            # Перезавантажуємо вікно, лише коли джерело справді змінилося:
            # смикати чат кожні кілька хвилин — гірше, ніж будь-яка автоматика.
            self.refresh_source()
        self._probe_timer.setInterval(self._probe_interval())

    def _probe_interval(self) -> int:
        """Ефір знайдено — перевіряємо рідко (раптом почався інший).
        Не знайдено — частіше, щоб чат з'явився невдовзі після старту стріму."""
        return 10 * 60 * 1000 if self.auto_video else 3 * 60 * 1000

    # --- джерело чату ---
    def active_sources(self) -> list:
        """Площадки, з яких зараз читаємо (для підпису в налаштуваннях)."""
        out = []
        if self.twitch_channel:
            out.append("Twitch")
        if self.kick_channel:
            out.append("Kick")
        if self.my_channel.strip() or self.yt_channel_id:
            out.append("YouTube")
        return out or (["чат сайту"] if self.site_url.strip() else ["нічого"])

    def refresh_source(self):
        """Переобчислює джерело чату і, якщо воно змінилося, відкриває його.

        Джерела — тільки ніки площадок із налаштувань. Спільна стрічка
        вмикається, щойно задано Twitch або Kick: двох чатів однією сторінкою
        YouTube не покажеш, та й іконка площадки потрібна саме тоді, коли
        джерело не одне.
        """
        if self.twitch_channel or self.kick_channel:
            self._start_feed()
            return
        self._stop_readers()
        url = resolve_chat_url(self._cli_url or "", self.auto_video, self.site_url)
        self.is_yt = is_youtube(url)
        self.bar.title.setText(self._title_for())
        if url != self.url or self.mode == "feed":
            self.mode = "web"
            self.url = url
            self._show_url()

    def _show_url(self):
        """Відкриває поточне посилання або пояснює, чого бракує.

        Порожня адреса — не помилка програми, а незаповнене налаштування:
        показати білу сторінку означало б залишити людину гадати, що зламалося.
        """
        if self.url:
            self.view.load(QUrl(self.url))
        else:
            self.view.setHtml(NO_SOURCE_HTML)

    def _start_feed(self):
        """Вмикає спільну стрічку і піднімає читачів для заданих площадок."""
        self._stop_readers()
        self.mode = "feed"
        self.is_yt = False
        self.url = ""
        self.bar.title.setText("Чат")
        self.feed.set_delay(self.chat_delay)
        self.feed.load()

        if self.twitch_channel:
            self._add_reader(chat_twitch.TwitchChat(self.twitch_channel, self))
        if self.kick_channel:
            self._add_reader(chat_kick.KickChat(self.kick_channel, self))
        # YouTube у стрічку беремо лише за заданим каналом: без нього шукати
        # нема чого, а посилання на чужий чат іде звичайним шляхом.
        yt_channel = self.my_channel.strip() or self.yt_channel_id
        if yt_channel:
            self._add_reader(chat_youtube.YouTubeChat(yt_channel, self))

    def _add_reader(self, reader):
        reader.event.connect(self._on_chat_event)
        reader.status.connect(self._on_reader_status)
        reader.start()
        self.readers.append(reader)

    def _stop_readers(self):
        for r in self.readers:
            r.stop()
        self.readers = []

    def _on_chat_event(self, event: dict):
        if self.mode == "feed" and self.feed is not None:
            self.feed.push(event)

    def _on_reader_status(self, text: str):
        if hasattr(self, "panel"):
            self.panel.set_chat_error(text)

    def _title_for(self) -> str:
        """Назва у смужці вікна: спершу програма, потім що саме показано.

        Просто «Немає джерела» читалося як помилка невідомо чия — на екрані ж
        не написано, чиє це вікно.
        """
        if self.mode == "feed":
            sources = ", ".join(self.active_sources())
            return "Hominka — %s" % sources if sources else "Hominka"
        if self.auto_video:
            return "Hominka — мій ефір"
        if self.url:
            return "Hominka — чат"
        return "Hominka — джерело не вибрано"

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

    # --- розмір тексту ---
    def zoom_in(self):
        self.set_zoom(self.zoom + 0.1)

    def zoom_out(self):
        self.set_zoom(self.zoom - 0.1)

    def set_zoom(self, z: float):
        self.zoom = max(0.5, min(3.0, round(z, 2)))
        self.view.setZoomFactor(self.zoom)
        self.panel.sync_zoom()
        self.save_config()

    # --- тло ---
    def set_bg_alpha(self, v: int):
        self.bg_alpha = max(0.0, min(1.0, v / 100))
        self._apply_border(self.accent)
        self.save_config()

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
        # Заставка збірки одним файлом: знімаємо її саме тут — вікно вже є.
        close_splash()

    def toggle_click_through(self):
        self.click_through = not self.click_through
        set_click_through(self, self.click_through)
        self.bar.set_locked(self.click_through)
        self.panel.set_accent(ACCENT_LOCKED if self.click_through else ACCENT_ACTIVE)
        self._apply_border(ACCENT_LOCKED if self.click_through else ACCENT_ACTIVE)

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

    def _load_config(self):
        cfg = {}
        try:
            # utf-8-sig, а не utf-8: варто відкрити config.json у «Блокноті» й
            # зберегти — Windows допише BOM, json.load на нього спіткнеться, і
            # всі налаштування мовчки скинуться на типові.
            with open(CONFIG_PATH, "r", encoding="utf-8-sig") as f:
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
        self.zoom = float(cfg.get("zoom", 1.0))
        self.bg_alpha = float(cfg.get("bg_alpha", 0.30))
        # Чат сайту — те, що людина вписала в ⚙. Аргумент командного рядка
        # сильніший: ним відкривають чужий чат для налагодження.
        self.site_url = (cfg.get("siteChatUrl") or "").strip()
        self.panel.site_edit.setText(self.site_url)
        self.custom_css = cfg.get("customCss") or ""
        if self.feed is not None:
            self.feed.custom_css = self.custom_css
        # Ми щойно оновилися? Тоді перше, що бачить людина, — за чим саме
        # закривалося вікно. Позначку одразу гасимо: показуємо один раз.
        was = (cfg.get("updatedTo") or "").strip()
        if was and updater.parse_version(APP_VERSION) >= updater.parse_version(was):
            self._updated_banner = (APP_VERSION, cfg.get("updatedNotes") or "")
        self.updated_to = ""
        self.updated_notes = ""
        if not self._cli_url:
            self.url = site_chat_url(self.site_url)
            self.is_yt = False
        # Канал YouTube, знайдений минулого разу: id не змінюється, тож не
        # ходимо за ним щоразу. Саму трансляцію не запам'ятовуємо — вона
        # застаріває швидше, ніж програма встигає закритися.
        self.yt_channel_id = cfg.get("youtubeChannelId", "")
        self.my_channel = cfg.get("myChannel", "")
        self.panel.channel_edit.setText(self.my_channel)
        self.chat_delay = int(cfg.get("chatDelay", 0) or 0)
        self.panel.delay.blockSignals(True)
        self.panel.delay.setValue(self.chat_delay)
        self.panel.delay_val.setText("%d с" % self.chat_delay)
        self.panel.delay.blockSignals(False)
        self.twitch_channel = cfg.get("twitchChannel", "")
        self.kick_channel = cfg.get("kickChannel", "")
        self.panel.twitch_edit.setText(self.twitch_channel)
        self.panel.kick_edit.setText(self.kick_channel)
        # Оновлення: канал, з якого читаємо, і чи перевіряти самим.
        ch = cfg.get("channel", updater.DEFAULT_CHANNEL)
        self.channel = ch if any(c[0] == ch for c in updater.CHANNELS) else updater.DEFAULT_CHANNEL
        self.installed_channel = cfg.get("installedChannel", "")
        self.auto_update = bool(cfg.get("autoUpdate", True))
        idx = self.panel.channel.findData(self.channel)
        if idx >= 0:
            self.panel.channel.blockSignals(True)
            self.panel.channel.setCurrentIndex(idx)
            self.panel.channel.blockSignals(False)
        self.panel.auto_upd.blockSignals(True)
        self.panel.auto_upd.setChecked(self.auto_update)
        self.panel.auto_upd.blockSignals(False)
        self.panel.set_status("Версія %s (%s)" % (APP_VERSION, updater.channel_label(self.channel)))
        # синхронізуємо панель з завантаженими значеннями
        self.panel.opacity.setValue(int(op * 100))
        self.panel.bg.setValue(int(self.bg_alpha * 100))
        self.panel.sync_zoom()
        self.bar.title.setText(self._title_for())
        self._apply_border(self.accent)

    def save_config(self):
        # дебаунс: реальний запис — через таймер (не на кожен resize-евент)
        self._save_timer.start()

    def _write_config(self):
        try:
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump({
                    "geometry": {"x": self.x(), "y": self.y(), "w": self.width(), "h": self.height()},
                    "opacity": round(self.windowOpacity(), 2),
                    "zoom": self.zoom,
                    "bg_alpha": round(self.bg_alpha, 2),
                    "youtubeChannelId": self.yt_channel_id,
                    "myChannel": self.my_channel,
                    "siteChatUrl": self.site_url,
                    "customCss": self.custom_css,
                    "updatedTo": self.updated_to,
                    "updatedNotes": self.updated_notes,
                    "chatDelay": self.chat_delay,
                    "twitchChannel": self.twitch_channel,
                    "kickChannel": self.kick_channel,
                    "channel": self.channel,
                    "installedChannel": self.installed_channel,
                    "autoUpdate": self.auto_update,
                }, f)
        except Exception:
            pass

    def closeEvent(self, e):
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
