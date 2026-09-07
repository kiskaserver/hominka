"""Читання чату СВОГО сайта — тим самим websocket, що й сама сторінка чату.

Навіщо окремий читач, коли чат сайту й так можна показати сторінкою. Бо
сторінка — це браузер, а він потрібен рівно для одного: намалювати чужу
верстку. Через це чат сайту не працював ані в грі (там показується наш кадр, а
не сайт), ані в нативному вікні (там браузера немає взагалі). З читачем чат
сайту стає таким самим джерелом, як Twitch чи Kick: приходить у спільну стрічку
і працює всюди.

Домовленість про повідомлення описана в server/internal/hub/protocol.go
(структура Outbound) — і вона майже збігається з нашою: нік, колір, значки,
справжні іконки значків, емоути, кому відповідають. Це не випадковість:
chatsources.py із самого початку робився дзеркалом серверного hub.

Ключів не треба: /ws — публічний бік для глядачів (той самий, яким користується
сторінка чату). Приватний /ws/overlay із донатами й віджетами ми НЕ чіпаємо.
"""

import json
from urllib.parse import urlsplit

from PySide6.QtCore import QObject, QTimer, QUrl, Signal
from PySide6.QtNetwork import QNetworkRequest
from PySide6.QtWebSockets import QWebSocket

from . import chatsources as cs

RECONNECT_MS = 6000

# Сайт стоїть за Cloudflare, а той відмовляє рукостисканню без звичайних
# браузерних заголовків. Ті самі, що вже вживає решта програми.
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36")

# Значки автора на сайті приходять готовим списком імен (той самий набір, що й
# у решті програми), тож перекладати нічого не треба — досить відсіяти чуже.
# Прапорці admin/mod старіші за список badges і трапляються окремо.
FLAG_BADGES = (("admin", "broadcaster"), ("mod", "mod"))


def ws_url(site_url: str) -> str:
    """З адреси сторінки чату — адреса його websocket.

    Людина вписує посилання на сторінку («https://stream.svitix.com/chat?...»),
    а сокет живе поруч, за /ws. Беремо тільки схему й хост: шлях і параметри
    стосуються сторінки, а не сокета.
    """
    raw = (site_url or "").strip()
    if not raw:
        return ""
    if "://" not in raw:
        raw = "https://" + raw
    try:
        u = urlsplit(raw)
    except ValueError:
        return ""
    if not u.netloc:
        return ""
    scheme = "wss" if u.scheme in ("https", "wss") else "ws"
    return "%s://%s/ws" % (scheme, u.netloc)


class SiteChat(QObject):
    """Читає чат сайту й віддає події у спільному вигляді."""

    event = Signal(dict)
    status = Signal(str)

    def __init__(self, site_url: str, parent=None):
        super().__init__(parent)
        self.url = ws_url(site_url)
        # Площадки, які ми читаємо САМІ. Сервер уміє мостити Twitch і YouTube у
        # свій чат; якщо ми вже маємо власного читача для тієї площадки, її
        # повідомлення прийдуть двічі. Тому такі просто пропускаємо.
        self.skip_sources = set()
        self.ws = QWebSocket()
        self.ws.connected.connect(self._on_open)
        self.ws.disconnected.connect(self._on_close)
        self.ws.textMessageReceived.connect(self._on_text)
        self._retry = QTimer(self)
        self._retry.setSingleShot(True)
        self._retry.timeout.connect(self.start)
        self._stopped = False

    def start(self):
        if self._stopped or not self.url:
            return
        req = QNetworkRequest(QUrl(self.url))
        req.setRawHeader(b"User-Agent", UA.encode())
        # Origin потрібен тому ж Cloudflare: сокет без нього виглядає як не з
        # браузера, і рукостискання відхиляється.
        req.setRawHeader(b"Origin", self._origin().encode())
        self.ws.open(req)

    def _origin(self) -> str:
        u = urlsplit(self.url)
        scheme = "https" if u.scheme == "wss" else "http"
        return "%s://%s" % (scheme, u.netloc)

    def stop(self):
        self._stopped = True
        self._retry.stop()
        self.ws.close()

    def _on_open(self):
        self.status.emit("")

    def _on_close(self):
        if not self._stopped:
            self._retry.start(RECONNECT_MS)

    # --- розбір ------------------------------------------------------------

    @staticmethod
    def _badges(d: dict):
        out = []
        for flag, name in FLAG_BADGES:
            if d.get(flag):
                out.append(name)
        for b in d.get("badges") or []:
            if isinstance(b, str) and b not in out:
                out.append(b)
        return out

    @staticmethod
    def _icons(d: dict):
        out = []
        for ic in d.get("badgeIcons") or []:
            if isinstance(ic, dict) and ic.get("id") and ic.get("url"):
                out.append({"id": ic["id"], "url": ic["url"]})
        return out

    @staticmethod
    def _emotes(d: dict):
        out = []
        for e in d.get("emotes") or []:
            if isinstance(e, dict) and e.get("code") and e.get("url"):
                out.append({"code": e["code"], "url": e["url"]})
        return out

    def _platform(self, source: str) -> str:
        """Звідки насправді прийшло повідомлення.

        Сервер мостить чужі чати у свій, і в кожному повідомленні пише, чиє
        воно. Показуємо це так само, як для власних читачів: значок площадки
        біля рядка має відповідати правді.
        """
        src = (source or "").strip()
        if src in ("", "site", "donation"):
            return "site"
        return src if src in ("twitch", "kick", "youtube") else "site"

    def _on_text(self, raw: str):
        try:
            d = json.loads(raw)
        except ValueError:
            return
        if not isinstance(d, dict):
            return
        t = d.get("type") or ""

        if t == "chat":
            self._chat(d)
        elif t == "donation":
            self._donation(d)
        elif t == "delete":
            # Сервер видаляє за числовим id — приводимо до того ж вигляду, що й
            # у наших повідомленнях.
            did = d.get("deleteId")
            if did:
                self.event.emit(cs.delete("site", str(did)))
        elif t == "system":
            text = (d.get("text") or "").strip()
            if text:
                self.event.emit(cs.system("site", text))
        # history / stats / title / users / reaction — це для сторінки чату, а
        # не для стрічки: у стрічці ані списку глядачів, ані назви ефіру немає.

    def _chat(self, d: dict):
        # Прибране автомодерацією до нас доходить лише в особистому вікні
        # стрімера (raw=1) — показувати його в чаті поверх гри ні до чого.
        if d.get("deleted"):
            return
        source = d.get("source") or ""
        platform = self._platform(source)
        if platform != "site" and platform in self.skip_sources:
            return          # цю площадку ми читаємо самі — не дублюємо

        nick = d.get("nick") or ""
        text = d.get("text") or ""
        if not text:
            return
        self.event.emit(cs.message(
            platform, nick, d.get("displayName") or nick, text,
            id=str(d.get("id") or ""),
            color=d.get("color") or "",
            badges=self._badges(d),
            badge_icons=self._icons(d),
            emotes=self._emotes(d),
            reply=d.get("replyTo") or "",
        ))

    def _donation(self, d: dict):
        """Донат — це теж повідомлення, просто з сумою."""
        amount = (d.get("amount") or "").strip()
        cur = (d.get("currency") or "").strip()
        if amount and cur:
            amount = "%s %s" % (amount, cur)
        nick = d.get("nick") or d.get("displayName") or "Аноним"
        text = d.get("text") or ""
        self.event.emit(cs.message(
            "site", nick, d.get("displayName") or nick, text,
            id=str(d.get("id") or ""),
            amount=amount or cur,
            emotes=self._emotes(d),
        ))
