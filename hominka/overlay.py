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
# wintypes існує лише на Windows: там, де його немає, сам імпорт кидає помилку,
# і модуль не завантажився б узагалі. Усе, що ним користується, і так під
# «if IS_WINDOWS».
if sys.platform == "win32":
    from ctypes import wintypes
else:
    wintypes = None

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QIcon
from PySide6.QtWidgets import QApplication, QFrame, QMainWindow, QVBoxLayout
from . import feed as chatfeed
from . import update as updater
from . import config
from .config import ConfigMixin
from . import fullscreen, x11
from .paths import BASE_DIR, resource_path
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
        self.frameless = False     # «без рамки»: лише повідомлення (див. look._apply_chrome)
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
        # Форсування композиції над повноекранною грою (див. compositor.py):
        # крихітне не-приховане вікно, що не дає DWM піти в незалежний flip, —
        # інакше чат зникає у грі (Hunt: Showdown). Створюємо, показуємо лише
        # коли попереду повноекранна гра.
        from .compositor import CompositionKeeper
        self._compositor = CompositionKeeper() if IS_WINDOWS else None

        # Чат поверх гри. Обидва шляхи — і окреме вікно DirectComposition, і
        # оверлей, вкладений у гру, — живить ОДИН нативний рендер: він малює чат
        # сам і сам кладе кадр у спільну память для overlay.dll. Грабера кадру з
        # браузера більше немає взагалі.
        self.dcomp_on = False
        # Чим малювати чат: "web" — браузером у цьому вікні (як було),
        # "native" — окремим процесом рендера.
        #
        # Читаємо ДО складання вікна, і саме ТУТ — єдине місце. Спокуса
        # прочитати цей ключ ще й у _load_config разом з рештою велика, але від
        # нього залежить, чи створювати браузер, а це рішення приймається
        # раніше: два читання неминуче розійшлися б.
        cfg = config.peek()
        self.renderer = "native" if (cfg.get("renderer") or "") == "native" else "web"
        # Нативний рендер бере на себе вікно чату лише в режимі стрічки. Умова
        # тут та сама, що в sources.refresh_source: стрічка вмикається, щойно
        # задано Twitch, Kick або чат сайту. У «веб-режимі» (чужа сторінка
        # YouTube) без браузера показувати нема чого, і там він лишається.
        self._native_window = (self.renderer == "native" and url is None and
                               bool((cfg.get("twitchChannel") or "").strip() or
                                    (cfg.get("kickChannel") or "").strip() or
                                    (cfg.get("siteChatUrl") or "").strip()))
        self._native = None
        self._fetcher = None

        # Справжній чат у грі через інжектор (native/): overlay.dll бере кадр зі
        # спільної памʼяті, куди його кладе нативний рендер.
        self.game_on = False
        # Позицію й розмір чату в грі задає САМЕ ВІКНО чату: куди поставив і як
        # розтягнув на моніторі — там і такого ж розміру в грі. Рахує це сам
        # рендер (DCompWindow::monitor_fraction) — він і є тим вікном. Тут
        # лишаються тільки прозорість і приховування від OBS.
        self.game_opacity = 235
        self.game_hide_obs = False   # чат у грі бачить лише стрімер, не OBS
        self._inject_pid = 0         # у який процес вклали DLL (0 = будь-який)

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
        # Браузер піднімаємо ЛІНИВО. Chromium — це шість процесів і сотні
        # мегабайтів памʼяті; коли чат малює нативний рендер, він не потрібен
        # узагалі, і не запустити його — головний виграш цього кроку.
        self.profile = None
        self.view = None
        if not self._native_window:
            self._ensure_web()
        self.feed = chatfeed.ChatFeed(self.view)

        self.setCentralWidget(self.frame)
        self.panel = SettingsPanel(self)
        self.grip = SizeGrip(self.frame, ACCENT_ACTIVE)

    def _ensure_web(self):
        """Створює сторінку чату, якщо її ще немає.

        Викликається звідусіль, де без браузера справді не обійтися: чат сайту,
        сторінка YouTube, пошук власного ефіру. У режимі стрічки з нативним
        рендером не викликається жодного разу — і Chromium не стартує.
        """
        if self.view is not None:
            return
        # Імпорт саме тут, а не вгорі файлу. Модулі QtWebEngine тягнуть за собою
        # бібліотеки Chromium на сотні мегабайтів ще до того, як зʼявиться хоч
        # одна сторінка. Коли чат малює нативний рендер, вони не потрібні
        # взагалі — і не завантажити їх дешевше, ніж завантажити й не вживати.
        from PySide6.QtWebEngineCore import QWebEnginePage
        from PySide6.QtWebEngineWidgets import QWebEngineView

        from .webprofile import build_profile

        # Профіль тримаємо в полі: сторінка живе лише поки живий профіль, і без
        # посилання Qt зносить його разом із входом у YouTube.
        self.profile = build_profile(self)
        self.view = QWebEngineView(self)
        self.view.setPage(QWebEnginePage(self.profile, self.view))
        self.view.page().setBackgroundColor(QColor(0, 0, 0, 0))
        self.view.setAttribute(Qt.WA_TranslucentBackground, True)
        self.view.loadFinished.connect(self._on_loaded)
        self._vbox.addWidget(self.view, 1)
        self.view.setZoomFactor(getattr(self, "zoom", 1.0) or 1.0)
        if getattr(self, "feed", None) is not None and self.feed.view is None:
            # Стрічка вже жила без сторінки — тепер вона в неї є.
            self.feed.view = self.view
            self.feed.load()

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
        # Конфіг уже прочитано (це робить _restore_and_open) — аж тепер відомо,
        # чи потрібен нативний рендер і чи є в нього бінар.
        self._setup_native_renderer()

        # Пошук власної трансляції: окрема прихована сторінка в тому ж профілі.
        # Створюємо ЛІНИВО (див. probe_live): без заданого каналу шукати нема
        # чого, а піднімати заради порожнього пошуку цілий браузер — тим паче.
        self.probe = None
        if self.view is not None and (self.my_channel.strip() or self.yt_channel_id):
            # Імпорт тут, а не вгорі: probe.py тягне за собою QtWebEngine, а
            # він потрібен лише коли браузер і так уже піднято.
            from .probe import LiveProbe
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

        # Швидкий таймер — тільки для повноекранної гри: часто пере-піднімаємо чат
        # (FSO-гра сидить у вищому z-band, одноразового підняття мало) і тримаємо
        # вікно-композитор-кипер, щоб DWM не йшов у незалежний flip і чат було
        # видно. Раз на секунду тут замало — гра встигає накрити на пів секунди.
        if IS_WINDOWS:
            self._over_game_timer = QTimer(self)
            self._over_game_timer.setInterval(350)
            self._over_game_timer.timeout.connect(self._keep_over_game)
            self._over_game_timer.start()

    def _setup_native_renderer(self):
        """Готує нативний рендер, якщо його ввімкнено в налаштуваннях.

        Не запускає процес: він піднімається разом із «чатом поверх гри», бо на
        робочому столі чат і далі малює вікно. Тут лише зв'язки — щоб той самий
        потік подій, що йде на сторінку, йшов і в рендер."""
        # Раніше тут стояла ще й перевірка на Windows: рендера під Linux просто
        # не існувало. Тепер існує (native/render/main_linux.cpp), і єдине, що
        # лишилося важливим, — чи просив користувач нативний шлях.
        if self.renderer != "native":
            return
        from .imagefetch import ImageFetcher
        from .nativerender import NativeRenderer
        self._native = NativeRenderer(self)
        if not self._native.available():
            # Бінаря немає (стара збірка) — тихо лишаємось на старому шляху.
            self._native = None
            self.renderer = "web"
            return
        # Качалка картинок працює у своїх потоках і кличе нас звідти. Писати в
        # канал звідти можна: у нативного рендера свій потік-писар із чергою.
        self._fetcher = ImageFetcher(lambda url, data: self._native.image(url, data))
        self._fetcher.start()
        self.feed.sink = self._push_native
        # Назад приходить те, що людина зробила у вікні оверлея. Сигнал Qt сам
        # перекине це з потоку-читача сюди, у потік вікна.
        self._native.event.connect(self._on_native_event)
        if self._native_window:
            # Вікно рендера тут — це і є вікно чату, а не додаток «поверх гри».
            # Тож умикаємо його одразу, не чекаючи тумблера.
            QTimer.singleShot(0, lambda: self.set_dcomp_overlay(True))

    def _push_native(self, event: dict):
        """Та сама подія, що йде на сторінку, — і в нативний рендер.

        Емоути й значки він малює з БАЙТІВ, які шлемо ми: у мережу той бік не
        ходить принципово. Доки картинка їде, рядок показує код емоута —
        рівно як чат виглядав, доки емоутів не було."""
        if self._native is None:
            return
        kind = event.get("kind")
        if kind == "delete":
            self._native.delete(event.get("id", ""))
            return
        if kind == "purge":
            self._native.purge(event.get("nick", ""))
            return
        if kind == "css":
            self._native.set_css(event.get("css", ""))
            return
        if kind == "layout":
            self._native.set_layout(event.get("layout") or [])
            return
        for e in event.get("emotes") or []:
            self._fetcher.want((e or {}).get("url", ""))
        for b in event.get("badgeIcons") or []:
            self._fetcher.want((b or {}).get("url", ""))
        self._native.message(event)

    def _sync_native_config(self):
        """Вигляд і розмір — щоб рендер малював рівно те саме, що вікно чату."""
        if self._native is None or not self._native.alive():
            return
        v = self.view
        w, h = (max(80, v.width()), max(60, v.height())) if v is not None             else (max(80, self.width()), max(60, self.height()))
        self._native.set_config(zoom=self.zoom, width=w, height=h,
                                opacity=self.windowOpacity(), bg_alpha=self.bg_alpha,
                                frameless=self.frameless)

    def _on_native_event(self, ev: dict):
        """Людина покрутила щось у вікні оверлея.

        Налаштування рендер сам не зберігає — він лише каже, що сталося, а
        єдиним джерелом істини лишається config.json. Так не буває двох правд і
        не треба вирішувати, чиє значення новіше.
        """
        t = ev.get("t")
        if t == "look":
            if "opacity" in ev:
                self.setWindowOpacity(float(ev["opacity"]))
            if "bg_alpha" in ev:
                self.bg_alpha = float(ev["bg_alpha"])
                self._apply_border(self.accent)
            if "zoom" in ev:
                self.zoom = round(float(ev["zoom"]), 2)
                if self.view is not None:
                    self.view.setZoomFactor(self.zoom)
                self.panel.sync_zoom()
            # Значення вже застосовані рендером — назад їх не шлемо, інакше
            # повзунок сіпався б під пальцем.
            self._native._cfg.update({k: ev[k] for k in ("opacity", "bg_alpha", "zoom")
                                      if k in ev})
            self.save_config()
            return
        if t == "lock":
            # Те саме поняття, що й у Qt-вікні (look.toggle_click_through):
            # замкнене вікно пропускає мишу крізь себе.
            self.click_through = bool(ev.get("on"))
            self._native._cfg["locked"] = self.click_through
            return
        if t == "geometry":
            # Вікно оверлея тепер і є вікно чату — його геометрію й зберігаємо.
            try:
                self.setGeometry(int(ev["x"]), int(ev["y"]), int(ev["w"]), int(ev["h"]))
            except (KeyError, ValueError, TypeError):
                return
            self._native._cfg.update({"width": int(ev["w"]), "height": int(ev["h"])})
            self.save_config()
            return
        if t == "settings":
            self.toggle_settings()
            return

    def _keep_over_game(self):
        """Тримає DirectComposition-оверлей рівно за вікном чату, поки тумблер
        увімкнено й вікно чату видиме. БЕЗ завʼязки на «попереду гра»: та перевірка
        була ненадійна (Alt-Tab — і спереду вже не гра, чат зникав). Оверлей
        клік-скрізь, а вікно чату під ним на тому ж місці, тож на робочому столі
        двоєння не видно, а взаємодія з вікном чату проходить крізь нього."""
        if not IS_WINDOWS or not self.dcomp_on:
            return
        if self._native is not None:
            # Нативний рендер: те саме місце, той самий сенс — вікно рендера
            # лежить рівно на view, тож на робочому столі двоєння не видно.
            try:
                if self._native_window:
                    # Вікно рендера саме собі хазяїн: людина тягає його за
                    # смужку, а Python лише зберігає результат. Пересувати його
                    # звідси означало б відбирати вікно з-під руки.
                    self._sync_native_config()
                elif self.isVisible():
                    v = self.view
                    tl = v.mapToGlobal(v.rect().topLeft())
                    self._native.place(tl.x(), tl.y())
                    self._sync_native_config()
                else:
                    self._native.hide()
            except Exception:
                pass
            return
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

    def set_chat_layout(self, layout):
        """Порядок частин рядка — у вікно чату негайно і в config.json.

        Стосується лише спільної стрічки: сторінку YouTube чи чат сайту малюємо
        не ми, і переставляти там нічого.
        """
        self.chat_layout = chatfeed.clean_layout(layout)
        self.save_config()
        if self.feed is not None:
            self.feed.set_layout(self.chat_layout)


    def _inject_custom_css(self):
        """Кладе свій CSS і на звичайну сторінку чату (сайт або YouTube).

        Селектори там чужі, але людина, яка полізла в CSS, з чужими розбереться
        — а от «мій CSS працює тільки в одному з трьох режимів» пояснити було б
        нічим.
        """
        if not self.custom_css or self.view is None:
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
        if self.view is None:
            return
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
            self.panel.cap_height()
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
        # Тягнемо вікно — притримуємо грабер (щоб рух не смикався) і рухаємо
        # dcomp-оверлей слідом ЩЕ під час руху, а не раз на 350 мс, щоб він не
        # відставав від вікна.
        if getattr(self, "dcomp_on", False):
            self._keep_over_game()


    def resizeEvent(self, e):
        super().resizeEvent(e)
        self.grip.move(self.width() - self.grip.width() - 3,
                       self.height() - self.grip.height() - 3)
        self.grip.raise_()
        if self.panel.isVisible():
            self._place_panel()
        if getattr(self, "dcomp_on", False):
            self._keep_over_game()
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
    def _sync_inject(self):
        """Каже рендеру, чи класти кадр у спільну память для overlay.dll.

        Читання кадру з відеокарти коштує грошей, тож рендер робить його ЛИШЕ
        поки інжект увімкнено. Розкладку чату в грі він рахує сам — за власним
        вікном і монітором, на якому те стоїть.
        """
        if self._native is None:
            return
        self._native.set_inject(self.game_on, pid=self._inject_pid,
                                opacity=self.game_opacity,
                                hide_obs=self.game_hide_obs)

    def set_game_hide_obs(self, on: bool):
        """Ховати чат у грі від OBS (лишається видним стрімеру на моніторі)."""
        self.game_hide_obs = bool(on)
        self._sync_inject()
        self.save_config()

    def set_game_overlay(self, on: bool):
        """Вмикає/вимикає кадр чату для оверлея, вкладеного в гру. Доступно в
        усіх каналах — безпеку несуть вимкнений за замовчуванням прапорець,
        попередження і відмова інжектора в онлайн-іграх (guard)."""
        self.game_on = bool(on)
        if self.game_on:
            # Кадр малює той самий рендер, що й вікно чату, — піднімаємо його,
            # якщо він ще не працює.
            if self._native is not None and not self._native.alive():
                self._native.start()
                self._native.set_css(self.custom_css)
                self._native.set_layout(self.chat_layout)
                self._sync_native_config()
            self._sync_inject()
            # Vulkan-гру не можна «вкласти» після старту — шар має бути на місці
            # ще до запуску гри. Реєструємо його, поки чат у грі ввімкнено; при
            # вимкненні/виході знімаємо, щоб не вантажився в чужі Vulkan-застосунки.
            try:
                from . import vklayer
                vklayer.register()
            except Exception:
                pass
        else:
            self._sync_inject()
            try:
                from . import vklayer
                vklayer.unregister()
            except Exception:
                pass
        self.save_config()

    def set_dcomp_overlay(self, on: bool):
        """Чат поверх гри через DirectComposition (без інжекту) — працює навіть у
        безрамковому повноекранному (Hunt), де звичайне вікно чату зникає, і
        лишається схованим від OBS. Той самий продюсер кадру, що й для інжекту;
        показує його окремий процес (dcomp.py). Позицію тримає _keep_over_game."""
        self.dcomp_on = bool(on)
        if self._native is not None:
            # Нативний шлях: ані грабера, ані спільної памʼяті — процес малює
            # чат сам із подій, які ми йому шлемо.
            try:
                if self.dcomp_on:
                    if not self._native.start():
                        self.dcomp_on = False
                        return
                    if self._native_window:
                        # Вікно рендера стає вікном чату: ставимо його туди, де
                        # мало бути Qt-вікно, і саме Qt-вікно ховаємо, щоб не
                        # двоїлося.
                        g = self.geometry()
                        self._native.set_config(width=g.width(), height=g.height())
                        self._native.place(g.x(), g.y())
                        self.hide()
                    # Стан, який рендер має знати ДО першого повідомлення.
                    self._native.set_css(self.custom_css)
                    self._native.set_layout(self.chat_layout)
                    self._sync_native_config()
                    self._native.set_enabled(True)
                    self._keep_over_game()
                else:
                    self._native.stop()
            except Exception as e:
                self.dcomp_on = False
                from .diag import log
                log("чат поверх гри: %r" % (e,))
            return

    def inject_game(self, hwnd: int):
        """Кладе overlay.dll у вікно hwnd і, якщо вдалося, вмикає продюсера."""
        from . import inject
        res = inject.inject(hwnd)
        if res.ok:
            # Малювати чат лише в ЦЬОМУ процесі: інакше він зʼявився б і в
            # сторонньому вікні, куди DLL могла потрапити раніше.
            self._inject_pid = res.pid
            self.set_game_overlay(True)
        return res

    def set_game_opacity(self, opacity: int):
        self.game_opacity = int(opacity)
        self._sync_inject()
        self.save_config()

    def set_keep_top(self, on: bool):
        self.keep_top = bool(on)
        self._topmost.enabled = self.keep_top
        self.save_config()

    def make_game_borderless(self, hwnd: int = 0):
        """Переводить ВИБРАНЕ вікно гри в безрамковий режим — на прохання людини.

        Вікно приходить зі списку в панелі (fullscreen.list_windows), а не з
        автовизначення переднього вікна: так людина точно бачить, що саме
        зробить безрамковим.
        """
        if not hwnd:
            return False
        return fullscreen.make_borderless(int(hwnd))

    def restore_game_window(self):
        return fullscreen.restore(fullscreen.changed_window())

    def toggle_fullscreen_opt(self, hwnd):
        """Вмикає/вимикає «без повноекранної оптимізації» для гри цього вікна —
        надійний спосіб повернути оверлей у безрамкових іграх, що ховають його в
        3D (Independent Flip). Повертає новий стан (True = FSO вимкнено) або None,
        якщо не вдалося (немає .exe чи доступу до реєстру)."""
        path = fullscreen.game_exe_path(int(hwnd))
        if not path:
            return None
        new_disabled = not fullscreen.fullscreen_opt_disabled(path)
        if not fullscreen.set_fullscreen_opt_disabled(path, new_disabled):
            return None
        return new_disabled

    def closeEvent(self, e):
        if getattr(self, "_compositor", None) is not None:
            try:
                self._compositor.close()
                self._compositor.deleteLater()
            except Exception:
                pass
        if getattr(self, "_native", None) is not None:
            try:
                self._native.stop()
            except Exception:
                pass
            if self._fetcher is not None:
                try:
                    self._fetcher.stop()
                except Exception:
                    pass
        if getattr(self, "_dcomp", None) is not None:
            try:
                self._dcomp.stop()
            except Exception:
                pass
        if getattr(self, "_dcomp_producer", None) is not None:
            try:
                self._dcomp_producer.close()
            except Exception:
                pass
        # Знімаємо Vulkan-шар з реєстру — щоб він не вантажився в чужі Vulkan-ігри
        # після того, як Hominka закрито.
        try:
            from . import vklayer
            vklayer.unregister()
        except Exception:
            pass
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
