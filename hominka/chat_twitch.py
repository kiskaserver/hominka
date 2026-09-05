"""
Читання чату Twitch — анонімно, без ключів і без входу.

Twitch віддає IRC поверх WebSocket, і читати чат може будь-хто: достатньо
представитися ніком виду justinfan<число>. Теги (IRCv3) дають усе інше —
значки, колір ніка, емоути, біти, id повідомлення.

Розбір тегів повторює серверний (server/internal/twitchirc/tags.go), зокрема
дві речі, на яких там уже спіткалися:
  • діапазони емоутів рахуються в СИМВОЛАХ, а не байтах — інакше кирилиця
    зсуває вирізку і замість емоута дістається сміття;
  • біти це гроші, і показувати їх звичайним рядком означає загубити донат.
"""

import random
import re

from PySide6.QtCore import QObject, QTimer, QUrl, Signal
from PySide6.QtWebSockets import QWebSocket

from . import chatsources as cs
from .badges import ICONS as BADGE_ICONS
from .thirdparty import EMOTES


IRC_URL = "wss://irc-ws.chat.twitch.tv:443"
EMOTE_CDN = "https://static-cdn.jtvnw.net/emoticons/v2/%s/default/dark/2.0"
RECONNECT_MS = 5000


def parse_tags(raw: str) -> dict:
    """@a=1;b=2 → {'a': '1', 'b': '2'} з розекрануванням значень."""
    out = {}
    for part in raw.split(";"):
        key, _, val = part.partition("=")
        out[key] = (val.replace(r"\s", " ").replace(r"\:", ";")
                    .replace(r"\r", "\r").replace(r"\n", "\n").replace("\\\\", "\\"))
    return out


def parse_line(line: str):
    """IRC-рядок → (теги, префікс, команда, параметри)."""
    tags, prefix = {}, ""
    s = line
    if s.startswith("@"):
        head, _, s = s.partition(" ")
        tags = parse_tags(head[1:])
    if s.startswith(":"):
        prefix, _, s = s[1:].partition(" ")
    cmd, _, rest = s.partition(" ")
    params = []
    while rest:
        if rest.startswith(":"):
            params.append(rest[1:])
            break
        head, _, rest = rest.partition(" ")
        params.append(head)
    return tags, prefix, cmd.upper(), params


def map_badges(tag: str) -> list:
    """Регалії Twitch → спільний набір (див. chatsources.BADGES)."""
    known = {"broadcaster": "broadcaster", "moderator": "mod", "vip": "vip",
             "subscriber": "sub", "founder": "sub", "partner": "verified",
             "staff": "staff", "admin": "staff", "global_mod": "staff",
             "artist-badge": "artist"}
    out = []
    for item in (tag or "").split(","):
        name = item.split("/")[0]
        mapped = known.get(name)
        if mapped and mapped not in out:
            out.append(mapped)
    return out


def parse_emotes(tag: str, text: str) -> list:
    """Тег emotes → картинки. Діапазони — в символах, не в байтах."""
    if not tag or not text:
        return []
    chars = list(text)
    out = []
    for item in tag.split("/"):
        eid, _, ranges = item.partition(":")
        if not eid or not ranges:
            continue
        first = ranges.split(",")[0]
        lo, _, hi = first.partition("-")
        try:
            a, b = int(lo), int(hi)
        except ValueError:
            continue
        if a < 0 or b < a or b >= len(chars):
            continue
        out.append({"code": "".join(chars[a:b + 1]), "url": EMOTE_CDN % eid})
    return out


def money(tags: dict):
    """Біти або платне закріплене повідомлення (Hype Chat)."""
    bits = tags.get("bits", "")
    if bits.isdigit() and int(bits) > 0:
        return "%s bits" % bits
    raw, cur = tags.get("pinned-chat-paid-amount", ""), tags.get("pinned-chat-paid-currency", "")
    if raw.isdigit() and cur:
        exp = tags.get("pinned-chat-paid-exponent", "0")
        try:
            value = int(raw) / (10 ** int(exp or 0))
        except (ValueError, ZeroDivisionError):
            return ""
        return ("%g %s" % (value, cur))
    return ""


class TwitchChat(QObject):
    """Одне підключення до чату каналу. Події йдуть сигналом event."""

    event = Signal(dict)
    status = Signal(str)          # порожньо = все добре

    def __init__(self, channel: str, parent=None):
        super().__init__(parent)
        self.channel = (channel or "").lower().lstrip("#")
        self.ws = QWebSocket()
        self.ws.connected.connect(self._on_open)
        self.ws.textMessageReceived.connect(self._on_text)
        self.ws.disconnected.connect(self._on_close)
        self.ws.errorOccurred.connect(lambda _e: self.status.emit(self.ws.errorString()))
        self._retry = QTimer(self)
        self._retry.setSingleShot(True)
        self._retry.timeout.connect(self.start)
        self._stopped = False

    def start(self):
        if self._stopped or not self.channel:
            return
        self.ws.open(QUrl(IRC_URL))

    def stop(self):
        self._stopped = True
        self._retry.stop()
        self.ws.close()

    def _on_open(self):
        self.status.emit("")
        # justinfan — анонімний читач. Пароль не потрібен, писати не можемо.
        for line in ("CAP REQ :twitch.tv/tags twitch.tv/commands",
                     "NICK justinfan%d" % random.randint(10000, 99999),
                     "JOIN #" + self.channel):
            self.ws.sendTextMessage(line)

    def _on_close(self):
        if not self._stopped:
            self._retry.start(RECONNECT_MS)

    def _on_text(self, text: str):
        for line in text.replace("\r\n", "\n").split("\n"):
            if line:
                self._handle(line)

    def _handle(self, line: str):
        if line.startswith("PING"):
            self.ws.sendTextMessage("PONG " + line.split(" ", 1)[-1])
            return
        tags, prefix, cmd, params = parse_line(line)
        if cmd == "PRIVMSG":
            self._privmsg(tags, prefix, params)
        elif cmd == "CLEARMSG":
            self.event.emit(cs.delete(cs.TWITCH, tags.get("target-msg-id", "")))
        elif cmd == "CLEARCHAT" and len(params) > 1:
            self.event.emit(cs.purge(cs.TWITCH, params[1].lower()))
        elif cmd == "USERNOTICE":
            self._usernotice(tags, params)
        elif cmd == "NOTICE":
            if params:
                self.status.emit(params[-1])
        elif cmd == "RECONNECT":
            self.ws.close()

    def _privmsg(self, tags, prefix, params):
        if len(params) < 2:
            return
        text = params[1]
        login = prefix.split("!")[0]
        if text.startswith("\x01ACTION ") and text.endswith("\x01"):
            text = "/me " + text[8:-1]
        amount = money(tags)
        if not text.strip() and not amount:
            return
        # Рідні емоути Twitch (тег emotes) плюс сторонні 7TV/BTTV/FFZ, знайдені
        # в тексті. room-id — числовий id каналу, за ним тягнуться набори.
        emotes = parse_emotes(tags.get("emotes", ""), text)
        emotes = EMOTES.append(emotes, "twitch", tags.get("room-id", ""), text)
        self.event.emit(cs.message(
            cs.TWITCH, login, tags.get("display-name") or login, text,
            id=tags.get("id", ""), color=tags.get("color", ""),
            badges=map_badges(tags.get("badges", "")),
            badge_icons=BADGE_ICONS.twitch(tags.get("room-id", ""), tags.get("badges", "")),
            emotes=emotes,
            reply=tags.get("reply-parent-display-name", ""),
            amount=amount,
            event="bits" if amount else "",
        ))

    def _usernotice(self, tags, params):
        """Підписки, рейди, оголошення — усе, що Twitch шле системним."""
        user = tags.get("display-name") or tags.get("login") or "Anonymous"
        kind = tags.get("msg-id", "")
        body = params[1] if len(params) > 1 else ""
        event = ""
        if kind == "raid":
            text = "%s привів рейд: %s глядачів" % (user, tags.get("msg-param-viewerCount", "?"))
            event = "raid"
        elif kind == "announcement":
            text = "%s: %s" % (user, body) if body else ""
            event = "announce"
        elif kind in ("sub", "resub", "subgift", "anonsubgift",
                      "submysterygift", "anonsubmysterygift",
                      "primepaidupgrade", "giftpaidupgrade", "anongiftpaidupgrade"):
            # Свій текст події Twitch уже зібрав — беремо його, а не переказуємо.
            text = tags.get("system-msg", "") or ("%s: підписка" % user)
            # Подарунок і власна підписка — різні приводи, і оформлюють їх
            # по-різному: одне вітають, друге дякують.
            event = "gift" if "gift" in kind else "sub"
        else:
            text = tags.get("system-msg", "")
        if text.strip():
            self.event.emit(cs.system(cs.TWITCH, text.strip(), event))


# Використовується тестами й налаштуваннями: чи схожий рядок на канал Twitch.
CHANNEL_RE = re.compile(r"^[A-Za-z0-9_]{3,25}$")
