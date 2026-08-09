"""
Chat Overlay — прозорий оверлей чату поверх гри, НЕВИДИМИЙ для OBS.

Навіщо: у тебе один монітор, і чат, який OBS накладає на трансляцію, ти сам не
бачиш. Ця програма показує сторінку чату окремим вікном поверх усіх ігор, але
Windows приховує це вікно від будь-якого захоплення екрана (OBS Display/Window/
Game Capture його НЕ бачить — WDA_EXCLUDEFROMCAPTURE, Windows 10 2004+ / 11).

Джерело чату (за спаданням пріоритету, див. resolve_chat_url):
  • посилання, вписане руками в ⚙, — явний вибір людини сильніший за автоматику
    (потрібне хіба для чужого чату);
  • ВЛАСНА трансляція: у ⚙ один раз названо свій канал, і програма сама
    знаходить активний ефір та відкриває його чат (див. LiveProbe);
  • інакше — чат сайту (CHAT_URL).

Входу в акаунт YouTube тут немає навмисне: Google не пускає у вбудований
браузер, а для пошуку трансляції вхід і не потрібен — сторінки каналу відкриті
всім.

Для YouTube-чату: вмикаємо гарний прозорий стиль (м'які рожево-фіолетові
плашки), ховаємо зайвий інтерфейс, перемикаємо чат у режим «Чат наживо»
(YT_ALL_MESSAGES_JS) і підсвічуємо звертання «@нік» (YT_MENTIONS_JS).

Можливості (панель ⚙):
  • свій канал (щоб чат трансляції знаходився сам);
  • посилання на чат (для чужого чату);
  • прозорість вікна (повзунок);
  • тло / затемнення підкладки під чатом (повзунок);
  • розмір тексту (A− / A+);
  • клік-крізь (миша йде в гру) — кнопка 🔓 або Ctrl+Alt+Space;
  • зміна розміру — за помітний куточок унизу праворуч;
  • запам'ятовує все у config.json (поряд з .exe).

Запуск:  python chat_overlay.py  [URL]
"""

import ctypes
import json
import os
import re
import sys
import tempfile
from ctypes import wintypes

from PySide6.QtCore import Qt, QUrl, QTimer, QPoint, QObject, Signal  # noqa
from PySide6.QtGui import QColor, QPainter, QPen, QIcon
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QFrame, QVBoxLayout, QHBoxLayout,
    QSizeGrip, QPushButton, QLabel, QSlider, QLineEdit, QComboBox, QCheckBox,
    QProgressBar, QWidget, QGraphicsDropShadowEffect,
)
from PySide6.QtWebEngineCore import QWebEngineProfile, QWebEnginePage
from PySide6.QtWebEngineWidgets import QWebEngineView

import chatfeed
import chatsources as cs
import chat_kick
import chat_twitch
import chat_youtube
import updater

# === Налаштування за замовчуванням ==========================================
APP_NAME = "Hominka"          # від укр. «гомін» — гомін голосів у чаті
APP_ICON = "hominka.ico"
APP_VERSION = "1.6.0"
APP_AUTHOR = "Mykyta Vinnyk"
# Ключ доступу до оверлеїв (?key=) обовʼязковий: без нього сервер відповідає 403.
# Перевипуск ключа в адмінці ламає це посилання — тоді треба оновити рядок нижче
# (або просто вставити новий URL у полі налаштувань — воно має пріоритет).
# raw=1 — особисте вікно: показує повідомлення ДО автомодерації (вирізане
# фільтром позначається червоним). Цей параметр НЕ можна ставити в OBS —
# джерело з ним покаже глядачам те, що фільтр прибрав.
CHAT_URL = "https://stream.svitix.com/overlay/chat?lang=uk&raw=1&key=YOUR_OVERLAY_KEY"


def resource_path(name: str) -> str:
    """Шлях до ресурсу (працює і в .exe через PyInstaller _MEIPASS)."""
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)

# config.json — поряд з .exe (або зі скриптом у dev-режимі), щоб налаштування
# зберігались і в зібраній програмі.
if getattr(sys, "frozen", False):
    BASE_DIR = os.path.dirname(sys.executable)
else:
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(BASE_DIR, "config.json")


def profile_dir() -> str:
    """Тека профілю браузера: кеш сторінок YouTube і його власні куки згоди.

    Поряд із програмою — щоб копію можна було перенести цілком; оновлення її не
    чіпає (підмінник копіює нове ПОВЕРХ старого). Якщо туди не пишеться
    (розпакували в Program Files), відступаємо в LOCALAPPDATA: інакше кожен
    запуск заново тягнув би кілька мегабайт скриптів YouTube.
    """
    here = os.path.join(BASE_DIR, "profile")
    try:
        os.makedirs(here, exist_ok=True)
        probe = os.path.join(here, ".writable")
        with open(probe, "w") as f:
            f.write("1")
        os.remove(probe)
        return here
    except OSError:
        alt = os.path.join(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir(),
                           "Hominka", "profile")
        os.makedirs(alt, exist_ok=True)
        return alt


PROFILE_DIR = profile_dir()

# UA звичайного Chrome. За замовчуванням QtWebEngine пише в UA сам себе, і на
# такий рядок Google реагує окремо — аж до відмови у вході.
CHROME_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
             "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36")


def build_profile(parent) -> QWebEngineProfile:
    """Постійний профіль браузера.

    Профіль за замовчуванням у Qt — «інкогніто»: нічого не зберігається, і
    кожен запуск качає сторінки YouTube з нуля. Іменований профіль лишає кеш і
    згоду на куки при собі. Входу в акаунт тут немає: Google не пускає у
    вбудований браузер, і програма навіть не пробує.
    """
    prof = QWebEngineProfile("hominka", parent)
    prof.setPersistentStoragePath(os.path.join(PROFILE_DIR, "storage"))
    prof.setCachePath(os.path.join(PROFILE_DIR, "cache"))
    prof.setPersistentCookiesPolicy(QWebEngineProfile.PersistentCookiesPolicy.ForcePersistentCookies)
    prof.setHttpUserAgent(CHROME_UA)
    return prof

ACCENT_ACTIVE = "#a855f7"   # рамка у звичайному режимі (фіолетова)
ACCENT_LOCKED = "#22c55e"   # рамка у режимі клік-крізь (зелена)

# === WinAPI константи =======================================================
WDA_EXCLUDEFROMCAPTURE = 0x00000011
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
WS_EX_LAYERED = 0x00080000
WM_HOTKEY = 0x0312
MOD_CONTROL = 0x0002
MOD_ALT = 0x0001
MOD_NOREPEAT = 0x4000
VK_SPACE = 0x20
HOTKEY_ID = 1

user32 = ctypes.windll.user32


def _hwnd(win) -> int:
    return int(win.winId())


def exclude_from_capture(win) -> bool:
    try:
        return bool(user32.SetWindowDisplayAffinity(_hwnd(win), WDA_EXCLUDEFROMCAPTURE))
    except Exception:
        return False


def set_click_through(win, enabled: bool):
    hwnd = _hwnd(win)
    ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    if enabled:
        ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED
    else:
        ex &= ~WS_EX_TRANSPARENT
    user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex)


# === Пошук власної трансляції ===============================================
#
# Програма — для стрімера, який читає СВІЙ чат, тож вставляти посилання перед
# кожним ефіром безглуздо: досить один раз назвати свій канал (⚙ → «Мій
# канал»), і трансляція знайдеться сама.
#
# Канал → трансляція: сторінка youtube.com/channel/<id>/live. Коли ефір іде,
# YouTube сам переадресовує на сторінку перегляду, і в ytInitialData лежить
# currentVideoEndpoint з id відео; ефіру немає — це просто сторінка каналу.
#
# Входу в акаунт тут немає й не треба: обидві сторінки відкриті всім. Це
# принципово — Google не пускає у вбудований браузер, і будувати на вході щось
# робоче не вийшло (див. історію: вікно passkey і «request is malformed»).
#
# Ходимо звичайними сторінками — ніяких ключів і ніяких квот.

# JS: id і назва каналу зі сторінки самого каналу (для поля «Мій канал»).
JS_CHANNEL_PAGE = r"""
(function () {
  var id = '', title = '';
  try { id = window.ytInitialData.metadata.channelMetadataRenderer.externalId || ''; } catch (e) {}
  try { title = window.ytInitialData.metadata.channelMetadataRenderer.title || ''; } catch (e) {}
  if (!id) {
    var l = document.querySelector('link[rel="canonical"]');
    if (l) { var m = l.href.match(/UC[0-9A-Za-z_\-]{22}/); if (m) id = m[0]; }
  }
  return JSON.stringify({id: id, title: title});
})();
"""

# JS: дістати id живої трансляції зі сторінки каналу /live.
JS_LIVE_VIDEO = r"""
(function () {
  var d = window.ytInitialData;
  if (!d) return '';
  var v = '';
  try { v = d.currentVideoEndpoint.watchEndpoint.videoId || ''; } catch (e) { v = ''; }
  if (!v) return '';
  // Сторінка перегляду буває і в завершеного ефіру, і в анонса — беремо тільки
  // те, що зараз в ефірі.
  var live = JSON.stringify(d).indexOf('"isLive":true') >= 0;
  // Назву каналу YouTube кладе по-різному: на сторінці каналу — в метаданих,
  // на сторінці перегляду (куди веде /live під час ефіру) — біля відео.
  var title = '';
  try { title = d.metadata.channelMetadataRenderer.title || ''; } catch (e) {}
  if (!title) {
    try { title = window.ytInitialPlayerResponse.videoDetails.author || ''; } catch (e) {}
  }
  if (!title) {
    var el = document.querySelector('ytd-video-owner-renderer #channel-name a, #owner #channel-name a');
    if (el) title = (el.textContent || '').trim();
  }
  var logged = null;
  try { logged = !!window.ytcfg.get('LOGGED_IN'); } catch (e) { logged = null; }
  return JSON.stringify({video: live ? v : '', title: title, logged: logged});
})();
"""


def channel_id_from(text: str) -> str:
    """Готовий UC-id із того, що вписали в «Мій канал» (або порожньо)."""
    m = re.search(r"UC[0-9A-Za-z_\-]{22}", text or "")
    return m.group(0) if m else ""


def channel_page_url(text: str) -> str:
    """Адреса сторінки каналу з того, що вписали: «@нік», посилання або нік.

    Резолвити @нік у UC-id доводиться сторінкою каналу — короткого способу в
    YouTube немає, зате цей працює й без входу.
    """
    raw = (text or "").strip()
    if raw.startswith("http://") or raw.startswith("https://"):
        return raw
    if raw.startswith("@"):
        return "https://www.youtube.com/" + raw
    if raw.startswith("youtube.com") or raw.startswith("www.youtube.com"):
        return "https://" + raw
    return "https://www.youtube.com/@" + raw.lstrip("@")


class LiveProbe(QObject):
    """Тихо шукає активну трансляцію заданого каналу.

    Працює на прихованій сторінці в тому ж профілі, тож вікно чату нічого не
    перемальовує.

    Дві сторінки, обидві відкриті всім:
      • сторінка каналу — щоб із «@нік» дістати UC-id (робиться один раз);
      • /channel/<id>/live — щоб дізнатися, чи йде ефір і який у нього id відео.

    На сторінки входу Google не заходимо ніколи: прихована сторінка, що туди
    потрапляє, викликає системне вікно passkey — воно вискакує посеред гри, і
    зрозуміти, звідки воно, неможливо.
    """

    # {"video", "channelId", "title"}
    result = Signal(dict)

    def __init__(self, profile: QWebEngineProfile, parent=None):
        super().__init__(parent)
        self.page = QWebEnginePage(profile, self)
        self.page.loadFinished.connect(self._on_loaded)
        # Підтверджувати щось на прихованій сторінці нікому: гасимо запити
        # passkey, якщо вони раптом виникнуть.
        if hasattr(self.page, "webAuthUxRequested"):
            self.page.webAuthUxRequested.connect(self._deny_webauth)

        self.channel_id = ""
        self.channel_title = ""
        self._step = ""          # resolve | live
        self._guard = QTimer(self)
        self._guard.setSingleShot(True)
        self._guard.setInterval(30000)   # сторінка не відповіла — не висимо вічно
        self._guard.timeout.connect(self._give_up)

    @property
    def busy(self) -> bool:
        return bool(self._step)

    @staticmethod
    def _deny_webauth(request):
        try:
            request.cancel()
        except Exception:
            pass

    def start(self, manual_channel: str = "", channel_id: str = ""):
        """manual_channel — те, що вписали в «Мій канал»; channel_id — вже
        відомий UC-id (щоб не резолвити «@нік» щоразу)."""
        if self._step:
            return
        manual = (manual_channel or "").strip()
        self.channel_id = channel_id_from(manual) or channel_id or self.channel_id
        if self.channel_id:
            self._go("live", "https://www.youtube.com/channel/%s/live?hl=en" % self.channel_id)
        elif manual:
            self._go("resolve", channel_page_url(manual))
        else:
            self._finish()       # каналу не задано — шукати нічого

    def _go(self, step: str, url: str):
        self._step = step
        self.page.setUrl(QUrl(url))
        self._guard.start()

    def _finish(self, video: str = ""):
        self._guard.stop()
        self._step = ""
        self.result.emit({
            "video": video,
            "channelId": self.channel_id,
            "title": self.channel_title,
        })

    def _give_up(self):
        self._step = ""
        self._finish()

    def _on_loaded(self, ok: bool):
        if not self._step:
            return
        if not ok:
            self._finish()
            return
        if self._step == "resolve":
            self.page.runJavaScript(JS_CHANNEL_PAGE, self._got_resolve)
        else:
            self.page.runJavaScript(JS_LIVE_VIDEO, self._got_live)

    def _parse(self, value):
        try:
            return json.loads(value) if value else {}
        except (ValueError, TypeError):
            return {}

    def _got_resolve(self, value):
        data = self._parse(value)
        cid = data.get("id") or ""
        self.channel_title = data.get("title") or self.channel_title
        if not cid:
            self._finish()   # такого каналу немає — це видно в налаштуваннях
            return
        self.channel_id = cid
        self._go("live", "https://www.youtube.com/channel/%s/live?hl=en" % cid)

    def _got_live(self, value):
        data = self._parse(value)
        self.channel_title = data.get("title") or self.channel_title
        self._finish(data.get("video") or "")


# === Розбір посилання на чат ================================================
def is_youtube(url: str) -> bool:
    return "youtube.com" in url or "youtu.be" in url


def is_chat_url(text: str) -> bool:
    """Чи годиться рядок як адреса чату.

    Вимоги мінімальні (чат буває не тільки в YouTube), але саме вони
    відрізняють адресу від випадково вставленого тексту: схема http(s) і
    непорожній хост із крапкою.
    """
    raw = (text or "").strip()
    if not raw.startswith("http://") and not raw.startswith("https://"):
        return False
    u = QUrl(raw)
    host = u.host()
    return u.isValid() and ("." in host or host == "localhost")


def popout_url(video_id: str) -> str:
    """Посилання на окреме вікно чату конкретної трансляції."""
    return f"https://www.youtube.com/live_chat?v={video_id}&is_popout=1"


def resolve_chat_url(raw: str, auto_video: str = "") -> str:
    """Куди дивитися вікну чату.

    Порядок навмисне такий:
      1. те, що людина вписала руками, — явний вибір сильніший за будь-яку
         автоматику (наприклад, читати чужу трансляцію);
      2. власна трансляція, знайдена після входу в YouTube, — головний режим:
         програма для того й є, щоб стрімер читав СВІЙ чат;
      3. чат сайту — коли входу немає або ефір не йде.
    """
    raw = (raw or "").strip()
    if not raw:
        return popout_url(auto_video) if auto_video else CHAT_URL
    if "youtube.com/live_chat" in raw:
        return raw
    if is_youtube(raw):
        m = re.search(r"(?:v=|youtu\.be/|/live/|/embed/|/shorts/)([A-Za-z0-9_-]{11})", raw)
        if m:
            return popout_url(m.group(1))
    return raw


# Гарний прозорий стиль для чату YouTube (м'які рожево-фіолетові плашки).
YT_STYLE_JS = r"""
(function () {
  var id = '__cuteChatStyle';
  if (document.getElementById(id)) return;
  var s = document.createElement('style');
  s.id = id;
  s.textContent = `
    html, body, yt-live-chat-renderer, #chat, #contents, #item-list,
    #items, #item-scroller, #item-offset, #content-pages,
    tp-yt-app-drawer, #primary { background: transparent !important; }

    /* прибираємо зайвий інтерфейс YouTube.
       #action-panel НЕ ховаємо: саме туди YouTube кладе панель реакцій, а поле
       введення й так прибране своїми селекторами нижче. */
    yt-live-chat-header-renderer,
    yt-live-chat-message-input-renderer,
    yt-live-chat-ticker-renderer,
    yt-live-chat-banner-manager,
    yt-live-chat-viewer-engagement-message-renderer,
    #ticker, #panel-pages, #separator,
    #input-panel, #live-chat-message-input,
    yt-live-chat-text-message-renderer #timestamp,
    tp-yt-paper-tooltip { display: none !important; }

    /* реакції (плаваючі емодзі та їхня панель) — лишаємо видимими, лише знімаємо
       темну підкладку, щоб не було смуги на прозорому оверлеї */
    #action-panel, #reaction-control-panel-overlay,
    yt-reaction-control-panel-view-model,
    yt-emoji-fountain-view-model, #emoji-fountain {
      background: transparent !important;
    }

    /* звичайні повідомлення — м'які плашки */
    yt-live-chat-text-message-renderer {
      padding: 5px 10px !important;
      margin: 5px 7px !important;
      background: rgba(30, 22, 42, 0.42) !important;
      border-radius: 14px !important;
      box-shadow: 0 1px 8px rgba(0, 0, 0, 0.35) !important;
    }
    yt-live-chat-text-message-renderer[author-type="owner"] {
      background: rgba(236, 72, 153, 0.30) !important;
    }
    yt-live-chat-text-message-renderer[author-type="moderator"] {
      background: rgba(99, 102, 241, 0.30) !important;
    }
    yt-live-chat-text-message-renderer[author-type="member"] {
      background: rgba(16, 185, 129, 0.26) !important;
    }
    yt-live-chat-text-message-renderer #author-name {
      color: #f9a8d4 !important;
      font-weight: 800 !important;
      text-shadow: 0 1px 2px rgba(0, 0, 0, 0.6) !important;
    }
    yt-live-chat-text-message-renderer #message {
      color: #fdf2ff !important;
      font-weight: 500 !important;
      text-shadow: 0 1px 3px rgba(0, 0, 0, 0.75) !important;
    }
    /* звертання «@нік» — щоб було видно, кому відповідають (див. YT_MENTIONS_JS) */
    .__ftsMention {
      color: #fde68a !important;
      background: rgba(250, 204, 21, 0.20) !important;
      padding: 0 3px !important;
      border-radius: 6px !important;
      font-weight: 800 !important;
    }

    /* емодзі (авторські та стандартні) — не ховаємо, гарний розмір */
    yt-live-chat-text-message-renderer #message img,
    #message img.emoji, img.emoji {
      height: 1.3em !important; width: auto !important;
      vertical-align: -0.28em !important; margin: 0 1px !important;
    }
    /* Значки біля імені (спонсорство, ранг, модератор, автор каналу).
       YouTube ставить їм vertical-align: sub — буквально опускає під рядок, і
       значок висить нижче ніка.
       Підіймаємо ЛИШЕ його, зсувом: position: relative нічого не переставляє,
       тому решта рядка лишається на місці. Спроба вирівняти рядок автора
       флексом (1.3.1) значок поставила рівно, але зламала базову лінію всієї
       строки — текст повідомлення поїхав на 4.5 px вище ніка.
       Заміряно на живому чаті: значок 1.59 px нижче → 0.05, текст 0 → 0. */
    yt-live-chat-author-badge-renderer {
      position: relative !important;
      top: -0.12em !important;
    }
    yt-live-chat-author-badge-renderer img,
    yt-live-chat-author-badge-renderer #image { height: 1em !important; width: auto !important; }

    /* Super Chat / Super Sticker — лишаємо кольори YouTube, лише округлюємо */
    yt-live-chat-paid-message-renderer,
    yt-live-chat-paid-sticker-renderer {
      border-radius: 14px !important; margin: 6px 7px !important;
      overflow: hidden !important;
      box-shadow: 0 2px 12px rgba(0, 0, 0, 0.45) !important;
    }
    /* Нові учасники / етапи членства (зелений акцент).
       legacy-paid — та сама подія у старому оформленні YouTube. */
    yt-live-chat-membership-item-renderer,
    yt-live-chat-legacy-paid-message-renderer {
      border-radius: 14px !important; margin: 6px 7px !important;
      background: rgba(16, 185, 129, 0.32) !important;
      box-shadow: 0 1px 10px rgba(0, 0, 0, 0.42) !important;
    }
    yt-live-chat-membership-item-renderer #header *,
    yt-live-chat-membership-item-renderer #message {
      color: #ecfdf5 !important; text-shadow: 0 1px 2px rgba(0, 0, 0, 0.6) !important;
    }
    /* Подаровані підписки — «X подарував N підписок» / «отримав подарунок» */
    yt-live-chat-sponsorships-gift-purchase-announcement-renderer,
    yt-live-chat-sponsorships-gift-redemption-announcement-renderer {
      border-radius: 14px !important; margin: 6px 7px !important;
      background: rgba(168, 85, 247, 0.32) !important;
      box-shadow: 0 1px 10px rgba(0, 0, 0, 0.42) !important;
    }
    yt-live-chat-sponsorships-gift-purchase-announcement-renderer #content *,
    yt-live-chat-sponsorships-gift-redemption-announcement-renderer * {
      color: #faf5ff !important; text-shadow: 0 1px 2px rgba(0, 0, 0, 0.6) !important;
    }
    /* Опитування від автора каналу.
       Живе в #action-panel — тій самій смузі, де панель реакцій, тому й
       лишилося без нашого оформлення, коли ми перестали її ховати. Своє
       оформлення в опитування розраховане на СВІТЛУ тему: питання майже чорне
       (#0f0f0f), підпис сірий — на прозорому оверлеї це нечитабельно. */
    yt-live-chat-poll-renderer {
      background: rgba(30, 22, 42, 0.55) !important;
      border-radius: 14px !important;
      margin: 6px 7px !important;
      padding: 6px 8px !important;
      box-shadow: 0 1px 10px rgba(0, 0, 0, 0.42) !important;
    }
    yt-live-chat-poll-renderer #poll-question {
      color: #fdf2ff !important;
      font-weight: 700 !important;
      text-shadow: 0 1px 3px rgba(0, 0, 0, 0.75) !important;
    }
    yt-live-chat-poll-renderer yt-live-chat-poll-header-renderer yt-formatted-string,
    yt-live-chat-poll-renderer #text-container,
    yt-live-chat-poll-renderer tp-yt-paper-item {
      color: #e9d5ff !important;
    }
    /* Смужка голосів — нашим фіолетовим, а не блакитним YouTube. */
    yt-live-chat-poll-renderer #vote-percentage-bar {
      background: rgba(168, 85, 247, 0.45) !important;
      border-radius: 8px !important;
    }
    /* Відповісти все одно не вийде: голос вимагає входу, якого в програмі
       немає. Клік лише відкинув би на сторінку входу — тож не приймаємо його
       зовсім, а результати показуємо. */
    yt-live-chat-poll-renderer #endpoint,
    yt-live-chat-poll-renderer yt-live-chat-poll-choice {
      pointer-events: none !important;
      cursor: default !important;
    }

    /* Страховка на решту цієї смуги. YouTube кладе в #action-panel і те, чого
       ми ще не бачили (Q&A, промо, покупки), і робить це у світлій темі —
       текст виходить майже чорний на прозорому оверлеї. Дешевше один раз
       сказати «тут текст світлий», ніж ловити кожну нову панель очима. */
    #action-panel yt-formatted-string,
    #action-panel yt-attributed-string,
    #action-panel .yt-core-attributed-string {
      color: #f0e6ff !important;
    }

    /* Збори коштів у чаті (fundraiser) — той самий фіолетовий акцент */
    yt-live-chat-donation-announcement-renderer {
      border-radius: 14px !important; margin: 6px 7px !important;
      background: rgba(168, 85, 247, 0.28) !important;
      color: #faf5ff !important;
    }
    /* Службові повідомлення: увімкнено повільний режим, видалено модератором,
       затримано автомодерацією. Це не «зайвий інтерфейс», а те, що стрімеру
       треба бачити, тож не ховаємо — лише робимо тьмянішими за живий чат. */
    yt-live-chat-mode-change-message-renderer,
    yt-live-chat-moderation-message-renderer,
    yt-live-chat-auto-mod-message-renderer {
      margin: 4px 7px !important;
      padding: 3px 8px !important;
      border-radius: 12px !important;
      background: rgba(63, 63, 70, 0.38) !important;
      color: #e4e4e7 !important;
      font-size: 0.92em !important;
    }
    ::-webkit-scrollbar { width: 0 !important; background: transparent !important; }
  `;
  (document.head || document.documentElement).appendChild(s);
})();
"""

# Перемикання чату YouTube у режим «Чат наживо».
#
# YouTube відкриває чат у режимі «Цікавий чат» (Top chat), а він показує НЕ ВСЕ:
# ховає схожі повідомлення, повідомлення нових акаунтів і все, що вважає спамом.
# Виглядає це як «чат майже мертвий», хоча люди пишуть. Сам перемикач лежить у
# шапці чату, яку ми ховаємо стилем вище, — тобто вручну його ще й не дістати.
#
# Пункти меню завжди йдуть у порядку [Цікавий чат, Чат наживо], тож беремо
# ОСТАННІЙ — це не залежить від мови інтерфейсу. Клік по вже вибраному пункту не
# робимо, тому повтор нічого не ламає, а періодичний повтор потрібен: коли
# YouTube перезавантажує чат, режим скидається назад на «цікавий».
YT_ALL_MESSAGES_JS = r"""
(function () {
  if (window.__ftsAllMessages) return;
  window.__ftsAllMessages = true;

  function switchOnce() {
    var box = document.querySelector('#view-selector');
    if (!box) return;
    var items = box.querySelectorAll('tp-yt-paper-listbox a');
    if (items.length < 2) return;
    var all = items[items.length - 1];
    if (all.getAttribute('aria-selected') === 'true') return;  // вже все видно
    all.click();
  }

  switchOnce();
  setInterval(switchOnce, 5000);
})();
"""

# Панель реакцій YouTube.
#
# Показати її нам, найпевніше, не доведеться: YouTube віддає реакції лише тим,
# хто увійшов у акаунт (анонімному внизу чату написано «Sign in to chat»), а
# входу в програмі немає — Google не пускає у вбудований браузер.
#
# Код лишаємо: він нічого не робить, поки панелі немає, а якщо вона колись
# з'явиться (YouTube не питає нас, коли міняє правила), — переносимо її у свій
# контейнер, щоб її не приховав той самий стиль, що прибирає поле вводу.
YT_REACTIONS_JS = r"""
(function () {
  if (window.__ftsReactionsOn) return;
  window.__ftsReactionsOn = true;

  var SEL = 'yt-reaction-control-panel-view-model,' +
            'yt-live-chat-reaction-control-panel-renderer,' +
            '#reaction-control-panel-overlay';

  function host() {
    var box = document.getElementById('__ftsReactions');
    if (box) return box;
    box = document.createElement('div');
    box.id = '__ftsReactions';
    box.style.cssText = 'position:fixed;right:8px;bottom:8px;z-index:2147483000;' +
      'display:flex;gap:6px;align-items:center;background:rgba(20,16,28,0.55);' +
      'border-radius:14px;padding:2px 6px;backdrop-filter:blur(2px);';
    document.body.appendChild(box);
    return box;
  }

  function move() {
    var panel = document.querySelector(SEL);
    if (!panel) return;
    var box = host();
    if (panel.parentElement !== box) box.appendChild(panel);
    panel.style.display = 'flex';
    panel.style.visibility = 'visible';
  }

  move();
  setInterval(move, 3000);
})();
"""

# Підсвічування звертань «@нік».
#
# У чаті YouTube немає гілок відповідей: люди відповідають одне одному, пишучи
# «@vasya Привіт!». У суцільному потоці повідомлень це губиться — незрозуміло,
# кому адресовано. Тому знаходимо звертання в тексті й загортаємо у span, який
# стиль вище фарбує жовтим.
#
# Чому вручну, а не innerHTML: сторінки YouTube працюють під Trusted Types, і
# присвоєння innerHTML там кидає помилку. Тому лише createElement/appendChild.
#
# Повідомлення додаються постійно, тож слухаємо MutationObserver. Вузли YouTube
# перевикористовує під нові повідомлення, тому запам'ятовуємо текст, який уже
# розмітили: змінився текст — розмічаємо заново.
YT_MENTIONS_JS = r"""
(function () {
  if (window.__ftsMentionsOn) return;
  window.__ftsMentionsOn = true;

  // «@нік»: перед @ не має бути літери (щоб пошта a@b.com не рахувалась
  // звертанням), а закінчуватись має літерою/цифрою (щоб кома чи крапка після
  // ніка не потрапили всередину).
  var RE = /(?<![\p{L}\p{N}_@.\-])@[\p{L}\p{N}_.\-]{0,31}[\p{L}\p{N}_]/gu;

  function paint(msg) {
    var text = msg.textContent || '';
    if (msg.__ftsText === text) return;   // цей текст уже розмічено
    msg.__ftsText = text;
    if (text.indexOf('@') < 0) return;

    var walker = document.createTreeWalker(msg, NodeFilter.SHOW_TEXT);
    var nodes = [];
    while (walker.nextNode()) nodes.push(walker.currentNode);

    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i], val = node.nodeValue || '';
      if (val.indexOf('@') < 0) continue;
      if (node.parentNode && node.parentNode.className === '__ftsMention') continue;

      var frag = document.createDocumentFragment(), last = 0, m;
      RE.lastIndex = 0;
      while ((m = RE.exec(val))) {
        if (m.index > last) frag.appendChild(document.createTextNode(val.slice(last, m.index)));
        var span = document.createElement('span');
        span.className = '__ftsMention';
        span.textContent = m[0];
        frag.appendChild(span);
        last = m.index + m[0].length;
      }
      if (!last) continue;
      if (last < val.length) frag.appendChild(document.createTextNode(val.slice(last)));
      node.parentNode.replaceChild(frag, node);
    }
  }

  function scan(root) {
    if (root.nodeType !== 1) return;
    if (root.id === 'message') paint(root);
    var list = root.querySelectorAll ? root.querySelectorAll('#message') : [];
    for (var i = 0; i < list.length; i++) paint(list[i]);
  }

  scan(document.documentElement);
  new MutationObserver(function (muts) {
    for (var i = 0; i < muts.length; i++) {
      var added = muts[i].addedNodes;
      for (var j = 0; j < added.length; j++) scan(added[j]);
    }
  }).observe(document.documentElement, { childList: true, subtree: true });
})();
"""

BTN_CSS = """
QPushButton {
    background: rgba(255,255,255,0.06);
    color: #e4e4e7;
    border: none;
    border-radius: 6px;
    font: 12px 'Segoe UI';
}
QPushButton:hover { background: rgba(255,255,255,0.16); color: #fff; }
QPushButton:pressed { background: rgba(255,255,255,0.24); }
"""

SLIDER_CSS = """
QSlider { max-height: 16px; }
QSlider::groove:horizontal {
    height: 4px; border-radius: 2px; background: rgba(255,255,255,0.18);
}
QSlider::sub-page:horizontal { background: %s; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 12px; height: 12px; margin: -5px 0; border-radius: 6px;
    background: #ffffff;
}
"""

# Вигляд вікна налаштувань. Один рядок стилю на все вікно: інакше кожен віджет
# обростає власним setStyleSheet, і зібрати з цього цілісний вигляд неможливо.
PANEL_CSS = """
#body {
    background: #17141f;
    border: 1px solid rgba(168,85,247,0.35);
    border-radius: 14px;
}
QLabel { color: #d4d4d8; font: 12px 'Segoe UI'; }
QLabel#title { color: #fafafa; font: 600 14px 'Segoe UI'; }
QLabel#cap {
    color: #c4b5fd; font: 600 11px 'Segoe UI';
    text-transform: uppercase; letter-spacing: 1px;
}
QLabel#field { color: #a1a1aa; font: 11px 'Segoe UI'; }
QLabel#dim   { color: #8b8b93; font: 11px 'Segoe UI'; }
QLabel#value { color: #e9d5ff; font: 600 11px 'Segoe UI'; }
QLabel#error { color: #fca5a5; font: 11px 'Segoe UI'; }

#card {
    background: rgba(255,255,255,0.035);
    border: 1px solid rgba(255,255,255,0.07);
    border-radius: 10px;
}

QLineEdit {
    background: rgba(255,255,255,0.06); color: #fafafa;
    border: 1px solid rgba(255,255,255,0.12); border-radius: 7px;
    padding: 5px 8px; font: 12px 'Segoe UI';
    selection-background-color: #a855f7;
}
QLineEdit:focus { border: 1px solid #a855f7; background: rgba(255,255,255,0.09); }

QPushButton#primary {
    background: #a855f7; color: #fff; border: none;
    border-radius: 7px; font: 600 12px 'Segoe UI';
}
QPushButton#primary:hover { background: #9333ea; }
QPushButton#primary:pressed { background: #7e22ce; }

QPushButton#ghost {
    background: rgba(255,255,255,0.06); color: #e4e4e7; border: none;
    border-radius: 7px; font: 12px 'Segoe UI';
}
QPushButton#ghost:hover { background: rgba(255,255,255,0.14); color: #fff; }
QPushButton#ghost:pressed { background: rgba(255,255,255,0.2); }

QComboBox {
    background: rgba(255,255,255,0.06); color: #fafafa;
    border: 1px solid rgba(255,255,255,0.12); border-radius: 7px;
    padding: 4px 8px; font: 12px 'Segoe UI';
    min-height: 18px;
}
QComboBox:hover { border: 1px solid #a855f7; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView {
    background: #17141f; color: #fafafa; selection-background-color: #a855f7;
    border: 1px solid rgba(255,255,255,0.14); outline: none; padding: 2px;
}

QCheckBox { color: #d4d4d8; font: 11px 'Segoe UI'; spacing: 7px; }
QCheckBox::indicator {
    width: 14px; height: 14px; border-radius: 4px;
    border: 1px solid rgba(255,255,255,0.28); background: rgba(255,255,255,0.06);
}
QCheckBox::indicator:hover { border: 1px solid rgba(168,85,247,0.7); }
/* Галочку в стилях не намалюєш без картинки, тому «увімкнено» — заповнений
   квадрат із темною серединою: помітно і не потребує зайвих файлів. */
QCheckBox::indicator:checked {
    background: #a855f7; border: 4px solid #17141f;
    width: 8px; height: 8px; border-radius: 6px;
}
"""


COMBO_CSS = """
QComboBox {
    background: rgba(255,255,255,0.07); color: #fafafa;
    border: 1px solid rgba(255,255,255,0.14); border-radius: 6px;
    padding: 3px 7px; font: 12px 'Segoe UI';
}
QComboBox:hover { border: 1px solid #a855f7; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView {
    background: #17131f; color: #fafafa; selection-background-color: #a855f7;
    border: 1px solid rgba(255,255,255,0.14); outline: none;
}
"""

CHECK_CSS = """
QCheckBox { color: #d4d4d8; font: 11px 'Segoe UI'; spacing: 6px; }
QCheckBox::indicator { width: 13px; height: 13px; border-radius: 3px;
    border: 1px solid rgba(255,255,255,0.28); background: rgba(255,255,255,0.06); }
QCheckBox::indicator:checked { background: #a855f7; border: 1px solid #a855f7; }
"""

PROGRESS_CSS = """
QProgressBar {
    background: rgba(255,255,255,0.10); border: none; border-radius: 4px;
    height: 8px; text-align: center; color: transparent;
}
QProgressBar::chunk { background: #a855f7; border-radius: 4px; }
"""

INPUT_CSS = """
QLineEdit {
    background: rgba(255,255,255,0.07); color: #fafafa;
    border: 1px solid rgba(255,255,255,0.14); border-radius: 6px;
    padding: 4px 7px; font: 12px 'Segoe UI';
    selection-background-color: #a855f7;
}
QLineEdit:focus { border: 1px solid #a855f7; }
"""


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

        lay.addWidget(self._label("Мій канал"))
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

        self.src_status = QLabel("", self)
        self.src_status.setObjectName("dim")
        self.src_status.setWordWrap(True)
        lay.addWidget(self.src_status)

        lay.addSpacing(2)
        lay.addWidget(self._label("Посилання на чат"))
        urow = QHBoxLayout()
        urow.setSpacing(6)
        self.url_edit = QLineEdit(self)
        self.url_edit.setPlaceholderText("порожньо = моя трансляція")
        self.url_edit.returnPressed.connect(self._apply_url)
        urow.addWidget(self.url_edit, 1)
        ok = QPushButton("OK", self)
        ok.setObjectName("primary")
        ok.setFixedSize(42, 28)
        ok.clicked.connect(self._apply_url)
        urow.addWidget(ok)
        lay.addLayout(urow)

        hint = QLabel("Потрібне лише для чужого чату — своя трансляція "
                      "знаходиться сама.", self)
        hint.setObjectName("dim")
        hint.setWordWrap(True)
        lay.addWidget(hint)

        self.url_error = QLabel("", self)
        self.url_error.setObjectName("error")
        self.url_error.setWordWrap(True)
        self.url_error.hide()
        lay.addWidget(self.url_error)

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
        manual_url = self.url_edit.text().strip()
        typed = self.channel_edit.text().strip()
        known = win.yt_channel_title or win.yt_channel_id

        if typed and not known:
            head = "Такого каналу не знайшли — перевірте @нік чи посилання."
        elif known:
            head = "Канал: %s." % known
        else:
            head = "Канал не вказано — автопошук трансляції вимкнено."

        if manual_url:
            tail = "Зараз показуємо посилання, вписане нижче."
        elif win.auto_video:
            tail = "Ефір іде — показуємо його чат."
        elif known:
            tail = "Ефіру немає — покажемо чат сайту, перемкнемось самі, щойно почнеться."
        else:
            tail = "Показуємо чат сайту."
        self.src_status.setText(head + " " + tail)

    def _apply_url(self):
        self.win.set_url(self.url_edit.text())

    def _apply_channel(self):
        self.win.set_my_channel(self.channel_edit.text())

    def _apply_extra(self):
        self.win.set_extra_channels(self.twitch_edit.text(), self.kick_edit.text())

    def set_url_error(self, text: str):
        self.url_error.setText(text)
        self.url_error.setVisible(bool(text))

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


class UpdateBanner(QFrame):
    """Смужка «є оновлення» під панеллю вікна.

    Не діалог і не спливаюче вікно: програма висить поверх гри, і модальне
    вікно посеред бою — гірше за будь-яке оновлення. Смужку видно, коли є що
    ставити, і вона зникає, щойно користувач вирішив.
    """

    def __init__(self, win: "Overlay"):
        super().__init__(win)
        self.win = win
        self.setObjectName("upd")
        self.setStyleSheet(
            "#upd { background: rgba(168,85,247,0.22);"
            " border-bottom: 1px solid rgba(168,85,247,0.45); }"
            "QLabel { color: #f5e9ff; font: 11px 'Segoe UI'; }"
        )
        lay = QHBoxLayout(self)
        lay.setContentsMargins(10, 5, 6, 5)
        lay.setSpacing(6)

        self.text = QLabel("", self)
        self.text.setWordWrap(True)
        self.text.setMinimumWidth(0)     # інакше QLabel вимагає ширини на весь рядок
        lay.addWidget(self.text, 1)

        self.bar = QProgressBar(self)
        self.bar.setStyleSheet(PROGRESS_CSS)
        self.bar.setFixedWidth(90)
        self.bar.hide()
        lay.addWidget(self.bar)

        self.go = QPushButton("Оновити", self)
        self.go.setFixedHeight(24)
        self.go.setStyleSheet(BTN_CSS + "QPushButton{background:#a855f7;color:#fff;font-weight:600;padding:0 8px;}"
                              "QPushButton:hover{background:#9333ea;}")
        self.go.clicked.connect(win.on_update_button)
        lay.addWidget(self.go)

        later = QPushButton("✕", self)
        later.setToolTip("Пізніше")
        later.setFixedSize(22, 22)
        later.setStyleSheet(BTN_CSS)
        later.clicked.connect(self.hide)
        lay.addWidget(later)

        self.hide()

    def show_release(self, rel):
        """У смужці — тільки версія і вид оновлення.

        Опис змін сюди не влазить: вікно чату вузьке, і довгий текст
        обрізався на півслові. Повністю він видно у ⚙ («Оновлення») і в
        підказці при наведенні.
        """
        self.text.setText("Є оновлення %s · %s" % (rel.version, updater.kind_label(rel.kind)))
        note = " ".join(rel.notes.split())
        self.text.setToolTip(note)
        self.setToolTip(note)
        self.bar.hide()
        self.go.setEnabled(True)
        self.go.setText("Оновити")
        self.show()

    def show_ready(self, rel):
        """Завантажено — тепер рішення за людиною.

        Раніше програма ставила оновлення одразу після завантаження й сама
        перезапускалася: посеред стріму це щонайменше неввічливо.
        """
        self.text.setText("Оновлення %s завантажено" % rel.version)
        self.bar.hide()
        self.go.setEnabled(True)
        self.go.setText("Встановити")
        self.show()

    def show_progress(self, done: int, total: int):
        self.bar.show()
        self.go.setEnabled(False)
        self.go.setText("Качаю…")
        if total > 0:
            self.bar.setRange(0, 100)
            self.bar.setValue(int(done * 100 / total))
        else:
            self.bar.setRange(0, 0)  # невідомий розмір — «біжуча» смужка

    def show_error(self, msg: str):
        self.text.setText("Оновлення не вдалося: " + msg)
        self.bar.hide()
        self.go.setEnabled(True)
        self.go.setText("Ще раз")
        self.show()


class Overlay(QMainWindow):
    def __init__(self, url: str | None = None):
        super().__init__()
        self.click_through = False
        self.zoom = 1.0            # масштаб тексту чату
        self.bg_alpha = 0.30       # затемнення підкладки під чатом
        self.accent = ACCENT_ACTIVE
        self.mode = "web"          # web = сторінка чату, feed = спільна стрічка
        self._cli_url = url        # URL з аргументу командного рядка (пріоритет)
        self.url = resolve_chat_url(url) if url else CHAT_URL
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

        self._load_config()
        if not self.panel.url_edit.text().strip() and (self.twitch_channel or self.kick_channel):
            self._start_feed()
        else:
            self.view.load(QUrl(self.url))

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
        self.panel.set_status("Доступно: %s. Що нового: %s" % (rel.title, rel.notes or "—"))
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
        """Ставить завантажене й виходить: підмінник чекає саме виходу."""
        if not self.downloaded:
            return
        try:
            app_dir = os.path.dirname(sys.executable) if getattr(sys, "frozen", False) else BASE_DIR
            # Канал запам'ятовуємо ДО перезапуску: після нього це вже інша збірка,
            # і без запису вона не знала б, з якої гілки прийшла.
            self.installed_channel = self.channel
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
    def refresh_source(self):
        """Переобчислює джерело чату і, якщо воно змінилося, відкриває його."""
        manual = self.panel.url_edit.text() if hasattr(self, "panel") else ""
        # Спільна стрічка вмикається, щойно задано Twitch або Kick: показати
        # два чати однією сторінкою YouTube неможливо, та й іконка площадки
        # потрібна саме тоді, коли джерел більше одного.
        if not manual.strip() and (self.twitch_channel or self.kick_channel):
            self._start_feed()
            return
        self._stop_readers()
        url = resolve_chat_url(manual, self.auto_video)
        self.is_yt = is_youtube(url)
        self.bar.title.setText(self._title_for(manual))
        if url != self.url or self.mode == "feed":
            self.mode = "web"
            self.url = url
            self.view.load(QUrl(url))

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
        if text and hasattr(self, "panel"):
            self.panel.set_url_error(text)

    def _title_for(self, manual: str) -> str:
        if manual.strip():
            return "YouTube" if self.is_yt else "Chat"
        if self.auto_video:
            return "Мій стрім"
        return "Chat"

    def set_url(self, raw: str):
        """Посилання з поля ⚙. Порожньо — режим автопошуку.

        Перевіряємо ДО того, як віддати вікну: невалідний рядок QWebEngineView
        показує сторінкою помилки, і виглядає це як «програма зламалася».
        """
        raw = (raw or "").strip()
        if raw and not is_chat_url(raw):
            self.panel.url_edit.setText(self.panel.url_edit.text().strip())
            self.panel.set_url_error("Не схоже на посилання. Потрібне http(s)://… "
                                     "або порожнє поле для автопошуку.")
            return
        self.panel.set_url_error("")
        if self.panel.url_edit.text() != raw:
            self.panel.url_edit.setText(raw)
        self.refresh_source()
        self.save_config()

    def _on_loaded(self, ok: bool):
        self.view.setZoomFactor(self.zoom)
        if ok and self.mode == "feed":
            self.feed.on_loaded()
            return
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
        if not exclude_from_capture(self):
            print("[chat-overlay] УВАГА: не вдалося виключити з захоплення "
                  "(потрібна Windows 10 2004+/11). OBS може бачити вікно.")
        self._register_hotkey()

    def toggle_click_through(self):
        self.click_through = not self.click_through
        set_click_through(self, self.click_through)
        self.bar.set_locked(self.click_through)
        self.panel.set_accent(ACCENT_LOCKED if self.click_through else ACCENT_ACTIVE)
        self._apply_border(ACCENT_LOCKED if self.click_through else ACCENT_ACTIVE)

    def _register_hotkey(self):
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
        # URL: аргумент командного рядка > config > дефолт
        if not self._cli_url:
            self.url = resolve_chat_url(cfg.get("url", "")) if cfg.get("url") else CHAT_URL
            self.is_yt = is_youtube(self.url)
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
        self.panel.url_edit.setText(cfg.get("url", "") if not self._cli_url else (self._cli_url or ""))
        self.panel.opacity.setValue(int(op * 100))
        self.panel.bg.setValue(int(self.bg_alpha * 100))
        self.panel.sync_zoom()
        self.bar.title.setText(self._title_for(self.panel.url_edit.text()))
        self._apply_border(self.accent)

    def save_config(self):
        # дебаунс: реальний запис — через таймер (не на кожен resize-евент)
        self._save_timer.start()

    def _write_config(self):
        try:
            # у config пишемо саме те, що ввів користувач (порожньо = свій чат)
            raw_url = self.panel.url_edit.text() if hasattr(self, "panel") else ""
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump({
                    "geometry": {"x": self.x(), "y": self.y(), "w": self.width(), "h": self.height()},
                    "opacity": round(self.windowOpacity(), 2),
                    "zoom": self.zoom,
                    "bg_alpha": round(self.bg_alpha, 2),
                    "url": raw_url,
                    "youtubeChannelId": self.yt_channel_id,
                    "myChannel": self.my_channel,
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


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else None
    os.environ.setdefault("QTWEBENGINE_CHROMIUM_FLAGS", "--enable-features=TranslucentWindows")
    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    app.setApplicationDisplayName(APP_NAME)
    app.setApplicationVersion(APP_VERSION)
    app.setOrganizationName(APP_AUTHOR)
    app.setWindowIcon(QIcon(resource_path(APP_ICON)))
    app.setQuitOnLastWindowClosed(True)
    win = Overlay(url)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    if sys.platform != "win32":
        print("Ця програма розрахована на Windows (виключення з захоплення екрана "
              "працює лише там).")
    main()
