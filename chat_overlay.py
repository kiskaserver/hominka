"""
Chat Overlay — прозорий оверлей чату поверх гри, НЕВИДИМИЙ для OBS.

Навіщо: у тебе один монітор, і чат, який OBS накладає на трансляцію, ти сам не
бачиш. Ця програма показує сторінку чату окремим вікном поверх усіх ігор, але
Windows приховує це вікно від будь-якого захоплення екрана (OBS Display/Window/
Game Capture його НЕ бачить — WDA_EXCLUDEFROMCAPTURE, Windows 10 2004+ / 11).

Джерело чату (⚙ → «Посилання на чат»):
  • порожньо / твоя сторінка stream.svitix.com — показуємо як є (без змін);
  • посилання на YouTube (трансляція або чат) — вмикаємо гарний прозорий стиль
    (м'які рожево-фіолетові плашки), ховаємо зайвий інтерфейс YouTube,
    перемикаємо чат у режим «Чат наживо» (див. YT_ALL_MESSAGES_JS) і
    підсвічуємо звертання «@нік» (див. YT_MENTIONS_JS).

Можливості (панель ⚙):
  • посилання на чат (YouTube / свій сайт);
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
from ctypes import wintypes

from PySide6.QtCore import Qt, QUrl, QTimer, QPoint  # noqa
from PySide6.QtGui import QColor, QPainter, QPen, QIcon
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QFrame, QVBoxLayout, QHBoxLayout,
    QSizeGrip, QPushButton, QLabel, QSlider, QLineEdit,
)
from PySide6.QtWebEngineWidgets import QWebEngineView

# === Налаштування за замовчуванням ==========================================
APP_NAME = "Hominka"          # від укр. «гомін» — гомін голосів у чаті
APP_ICON = "hominka.ico"
APP_VERSION = "1.0.3"
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


# === Розбір посилання на чат ================================================
def is_youtube(url: str) -> bool:
    return "youtube.com" in url or "youtu.be" in url


def resolve_chat_url(raw: str) -> str:
    """Порожньо → твоя сторінка. YouTube (будь-яка форма) → popout live_chat.
    Інше — віддаємо як є."""
    raw = (raw or "").strip()
    if not raw:
        return CHAT_URL
    if "youtube.com/live_chat" in raw:
        return raw
    if is_youtube(raw):
        m = re.search(r"(?:v=|youtu\.be/|/live/|/embed/|/shorts/)([A-Za-z0-9_-]{11})", raw)
        if m:
            return f"https://www.youtube.com/live_chat?v={m.group(1)}&is_popout=1"
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
    /* значки-бейджі учасника біля імені (кастомні емодзі каналу) */
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


class SettingsPanel(QFrame):
    """Випадна панель налаштувань (⚙): посилання, прозорість, тло, шрифт."""

    def __init__(self, win: "Overlay"):
        # Окреме верхнє вікно — інакше нативний QWebEngineView малює поверх
        # панелі і видно лише обрізаний край (звідси «порізане ОК»).
        super().__init__(None)
        self.win = win
        self.setWindowFlags(
            Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setObjectName("panel")
        # Фон малюємо у paintEvent (надійніше за стиль на верхньому вікні —
        # інакше фон не прокрашувався і текст висів на білому). Тут лише текст.
        self.setStyleSheet("QLabel { color: #d4d4d8; font: 11px 'Segoe UI'; }")
        self.setFixedWidth(288)

        lay = QVBoxLayout(self)
        lay.setContentsMargins(12, 11, 12, 12)
        lay.setSpacing(6)

        # --- Посилання на чат ---
        lay.addWidget(self._cap("Посилання на чат"))
        urow = QHBoxLayout()
        urow.setSpacing(6)
        self.url_edit = QLineEdit(win.url, self)
        self.url_edit.setStyleSheet(INPUT_CSS)
        self.url_edit.setPlaceholderText("YouTube-посилання або порожньо = твій чат")
        self.url_edit.returnPressed.connect(self._apply_url)
        urow.addWidget(self.url_edit, 1)
        ok = QPushButton("OK", self)
        ok.setFixedSize(38, 28)
        ok.setStyleSheet(BTN_CSS + "QPushButton{background:#a855f7;color:#fff;font-weight:600;}"
                         "QPushButton:hover{background:#9333ea;}")
        ok.clicked.connect(self._apply_url)
        urow.addWidget(ok)
        lay.addLayout(urow)
        hint = QLabel("YouTube → гарний прозорий стиль і всі повідомлення "
                      "(не «цікавий чат»). Порожньо/свій сайт → без змін.", self)
        hint.setStyleSheet("color:#8b8b93; font:10px 'Segoe UI';")
        hint.setWordWrap(True)
        lay.addWidget(hint)

        lay.addSpacing(4)

        # --- Прозорість вікна ---
        lay.addWidget(self._cap("Прозорість вікна"))
        self.opacity, self.op_pct = self._slider_row(lay, 25, 100, int(win.windowOpacity() * 100),
                                                     self._on_opacity)

        # --- Тло (затемнення) ---
        lay.addWidget(self._cap("Тло під чатом (затемнення)"))
        self.bg, self.bg_pct = self._slider_row(lay, 0, 85, int(win.bg_alpha * 100),
                                                self._on_bg)

        lay.addSpacing(4)

        # --- Розмір тексту ---
        lay.addWidget(self._cap("Розмір тексту"))
        frow = QHBoxLayout()
        frow.setSpacing(6)
        minus = QPushButton("A−", self)
        minus.setFixedSize(34, 26)
        minus.setStyleSheet(BTN_CSS)
        minus.clicked.connect(win.zoom_out)
        frow.addWidget(minus)
        self.zoom_lbl = QLabel(f"{int(win.zoom * 100)}%", self)
        self.zoom_lbl.setAlignment(Qt.AlignCenter)
        self.zoom_lbl.setFixedWidth(46)
        self.zoom_lbl.setStyleSheet("color:#a1a1aa; font:11px 'Segoe UI';")
        frow.addWidget(self.zoom_lbl)
        plus = QPushButton("A+", self)
        plus.setFixedSize(34, 26)
        plus.setStyleSheet(BTN_CSS)
        plus.clicked.connect(win.zoom_in)
        frow.addWidget(plus)
        frow.addStretch(1)
        lay.addLayout(frow)

        self.hide()

    def _cap(self, text: str) -> QLabel:
        lab = QLabel(text, self)
        lab.setStyleSheet("color:#e9d5ff; font:600 11px 'Segoe UI';")
        return lab

    def _slider_row(self, parent_lay, lo, hi, val, cb):
        row = QHBoxLayout()
        row.setSpacing(8)
        sld = QSlider(Qt.Horizontal, self)
        sld.setRange(lo, hi)
        sld.setValue(val)
        sld.setStyleSheet(SLIDER_CSS % ACCENT_ACTIVE)
        sld.valueChanged.connect(cb)
        row.addWidget(sld, 1)
        pct = QLabel(f"{val}%", self)
        pct.setFixedWidth(36)
        pct.setStyleSheet("color:#a1a1aa; font:11px 'Segoe UI';")
        row.addWidget(pct)
        parent_lay.addLayout(row)
        return sld, pct

    def _apply_url(self):
        self.win.set_url(self.url_edit.text())

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
        exclude_from_capture(self)  # OBS не бачить і панель налаштувань

    def paintEvent(self, e):
        # Гарантований тёмний фон із заокругленням та акцентною рамкою.
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(QPen(QColor(168, 85, 247, 120), 1))
        p.setBrush(QColor(18, 16, 24, 252))
        r = self.rect().adjusted(0, 0, -1, -1)
        p.drawRoundedRect(r, 12, 12)


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


class Overlay(QMainWindow):
    def __init__(self, url: str | None = None):
        super().__init__()
        self.click_through = False
        self.zoom = 1.0            # масштаб тексту чату
        self.bg_alpha = 0.30       # затемнення підкладки під чатом
        self.accent = ACCENT_ACTIVE
        self._cli_url = url        # URL з аргументу командного рядка (пріоритет)
        self.url = resolve_chat_url(url) if url else CHAT_URL
        self.is_yt = is_youtube(self.url)

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

        self.view = QWebEngineView(self)
        self.view.page().setBackgroundColor(QColor(0, 0, 0, 0))
        self.view.setAttribute(Qt.WA_TranslucentBackground, True)
        self.view.loadFinished.connect(self._on_loaded)
        vbox.addWidget(self.view, 1)

        self.setCentralWidget(self.frame)

        self.panel = SettingsPanel(self)

        self.grip = SizeGrip(self.frame, ACCENT_ACTIVE)

        self._load_config()
        self.view.load(QUrl(self.url))

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

    # --- джерело чату ---
    def set_url(self, raw: str):
        self.url = resolve_chat_url(raw)
        self.is_yt = is_youtube(self.url)
        self.bar.title.setText("YouTube" if self.is_yt else "Chat")
        if self.panel.url_edit.text() != raw:
            self.panel.url_edit.setText(raw)
        self.view.load(QUrl(self.url))
        self.save_config()

    def _on_loaded(self, ok: bool):
        self.view.setZoomFactor(self.zoom)
        if ok and self.is_yt:
            # гарний прозорий стиль поверх YouTube-чату
            self.view.page().runJavaScript(YT_STYLE_JS)
            # ...показуємо ВСІ повідомлення, а не «цікаві»...
            self.view.page().runJavaScript(YT_ALL_MESSAGES_JS)
            # ...і підсвічуємо «@нік», щоб було видно, кому відповідають
            self.view.page().runJavaScript(YT_MENTIONS_JS)

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
        # під кнопкою ⚙, вирівняно по правому краю, у глобальних координатах
        g = self.bar.gear
        anchor = g.mapToGlobal(QPoint(g.width(), g.height()))
        x, y = anchor.x() - self.panel.width(), anchor.y() + 6
        scr = self.screen().availableGeometry() if self.screen() else None
        if scr:
            x = max(scr.left() + 4, min(x, scr.right() - self.panel.width() - 4))
            y = max(scr.top() + 4, min(y, scr.bottom() - self.panel.height() - 4))
        self.panel.move(x, y)

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
        self.bg_alpha = max(0.0, min(0.85, v / 100))
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
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
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
        # синхронізуємо панель з завантаженими значеннями
        self.panel.url_edit.setText(cfg.get("url", "") if not self._cli_url else (self._cli_url or ""))
        self.panel.opacity.setValue(int(op * 100))
        self.panel.bg.setValue(int(self.bg_alpha * 100))
        self.panel.sync_zoom()
        self.bar.title.setText("YouTube" if self.is_yt else "Chat")
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
                }, f)
        except Exception:
            pass

    def closeEvent(self, e):
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
