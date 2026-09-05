"""
Читання чату Kick — теж без ключів.

Kick роздає чат через Pusher (той самий сокет, що й його сайт). Потрібен лише
chatroom_id каналу, а він дістається одним запитом до kick.com/api/v2.

Розбір подій повторює серверний (server/internal/kickchat/events.go): емоути
Kick пише прямо в текст розміткою [emote:37226:catJAM], і без розбору зритель
приїжджає рядком, довшим за саме повідомлення.
"""

import json
import re
import urllib.request
from threading import Thread

from PySide6.QtCore import QObject, QTimer, QUrl, Signal
from PySide6.QtWebSockets import QWebSocket

from . import chatsources as cs
from .badges import ICONS as BADGE_ICONS
from .thirdparty import EMOTES

# Публічний ключ Pusher у Kick (не секрет — зашитий у клієнті сайту).
PUSHER_URL = ("wss://ws-us2.pusher.com/app/32cbd69e4b950bf97679"
              "?protocol=7&client=js&version=8.4.0&flags=SSL")
CHANNEL_API = "https://kick.com/api/v2/channels/"
EMOTE_URL = "https://files.kick.com/emotes/%s/fullsize"
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36")
RECONNECT_MS = 6000
PING_MS = 100000

RE_EMOTE = re.compile(r"\[emote:(\d+):([^\]]*)\]")

EVT_MSG = r"App\Events\ChatMessageEvent"
EVT_SUB = r"App\Events\SubscriptionEvent"
EVT_GIFT = r"App\Events\GiftedSubscriptionsEvent"
EVT_DELETED = r"App\Events\MessageDeletedEvent"
EVT_BANNED = r"App\Events\UserBannedEvent"
EVT_HOST = r"App\Events\StreamHostEvent"
EVT_PINNED = r"App\Events\PinnedMessageCreatedEvent"
EVT_REWARD = r"App\Events\RewardRedeemedEvent"


def extract_emotes(content: str):
    """[emote:id:name] → (':name:', картинка). Текст лишається читабельним."""
    if "[emote:" not in content:
        return content, []
    found = []

    def swap(m):
        eid, name = m.group(1), m.group(2) or m.group(1)
        code = ":%s:" % name
        found.append({"code": code, "url": EMOTE_URL % eid})
        return code

    return RE_EMOTE.sub(swap, content), found


def map_badges(identity: dict) -> list:
    known = {"broadcaster": "broadcaster", "moderator": "mod", "vip": "vip",
             "subscriber": "sub", "founder": "sub", "verified": "verified",
             "staff": "staff", "og": "og"}
    out = []
    for b in (identity or {}).get("badges") or []:
        mapped = known.get((b or {}).get("type"))
        if mapped and mapped not in out:
            out.append(mapped)
    return out


def nick_of(sender: dict) -> str:
    """Ключ автора — той самий, що й для бану, тож рахується в одному місці."""
    name = (sender or {}).get("username") or ""
    nick = name.strip().lower().replace(" ", "_")
    return nick or ((sender or {}).get("slug") or "").lower()


class KickChat(QObject):
    """Одне підключення до чату каналу Kick."""

    event = Signal(dict)
    status = Signal(str)
    # Сигнал сам переносить виклик у потік Qt. Через QTimer.singleShot із
    # робочого потоку це не працює зовсім: там немає циклу подій, таймер не
    # спрацьовує, і сокет мовчки не відкривається.
    _resolved = Signal()

    def __init__(self, channel: str, parent=None):
        super().__init__(parent)
        self.channel = (channel or "").strip().lower()
        self.chatroom = 0
        self.kick_id = 0        # числовий id каналу — для сторонніх емоутів 7TV
        self.ws = QWebSocket()
        self.ws.connected.connect(self._on_open)
        self.ws.textMessageReceived.connect(self._on_text)
        self.ws.disconnected.connect(self._on_close)
        self.ws.errorOccurred.connect(lambda _e: self.status.emit(self.ws.errorString()))
        self._retry = QTimer(self)
        self._retry.setSingleShot(True)
        self._retry.timeout.connect(self.start)
        self._ping = QTimer(self)
        self._ping.setInterval(PING_MS)
        self._ping.timeout.connect(self._send_ping)
        self._resolved.connect(self._after_resolve)
        self._stopped = False

    def start(self):
        if self._stopped or not self.channel:
            return
        if self.chatroom:
            self.ws.open(QUrl(PUSHER_URL))
            return
        # id кімнати тягнемо в окремому потоці: це звичайний HTTP, і блокувати
        # ним вікно чату не можна.
        Thread(target=self._resolve, daemon=True).start()

    def stop(self):
        self._stopped = True
        self._retry.stop()
        self._ping.stop()
        self.ws.close()

    def _resolve(self):
        try:
            req = urllib.request.Request(CHANNEL_API + self.channel,
                                         headers={"User-Agent": UA, "Accept": "application/json"})
            data = json.loads(urllib.request.urlopen(req, timeout=20).read())
            self.chatroom = int((data.get("chatroom") or {}).get("id") or 0)
            # id каналу (не кімнати) — саме його чекає 7TV для набору Kick.
            self.kick_id = int(data.get("id") or 0)
        except Exception as e:      # мережа, Cloudflare, немає такого каналу
            self.status.emit("Kick: не вдалося знайти канал (%s)" % e)
            self.chatroom = 0
        self._resolved.emit()      # далі — вже в потоці Qt

    def _after_resolve(self):
        if self._stopped:
            return
        if not self.chatroom:
            self._retry.start(RECONNECT_MS * 5)
            return
        self.status.emit("")
        self.ws.open(QUrl(PUSHER_URL))

    def _on_open(self):
        self.ws.sendTextMessage(json.dumps({
            "event": "pusher:subscribe",
            "data": {"auth": "", "channel": "chatrooms.%d.v2" % self.chatroom},
        }))
        self._ping.start()

    def _send_ping(self):
        self.ws.sendTextMessage(json.dumps({"event": "pusher:ping", "data": {}}))

    def _on_close(self):
        self._ping.stop()
        if not self._stopped:
            self._retry.start(RECONNECT_MS)

    def _on_text(self, raw: str):
        try:
            frame = json.loads(raw)
        except ValueError:
            return
        name = frame.get("event") or ""
        # Поле data у Pusher — це JSON-РЯДОК усередині JSON.
        try:
            data = json.loads(frame.get("data") or "{}")
        except (ValueError, TypeError):
            data = {}

        if name == "pusher:ping":
            self.ws.sendTextMessage(json.dumps({"event": "pusher:pong", "data": {}}))
        elif name == EVT_MSG:
            self._message(data)
        elif name == EVT_SUB:
            user = data.get("username") or "Anonymous"
            months = data.get("months") or 0
            self.event.emit(cs.system(cs.KICK, "%s підписався%s" % (
                user, " (%d міс.)" % months if months else ""), "sub"))
        elif name == EVT_GIFT:
            gifter = data.get("gifter_username") or "Anonymous"
            n = len(data.get("gifted_usernames") or []) or 1
            self.event.emit(cs.system(cs.KICK, "%s подарував %d підписк(и)" % (gifter, n), "gift"))
        elif name == EVT_DELETED:
            mid = ((data.get("message") or {}).get("id")) or ""
            if mid:
                self.event.emit(cs.delete(cs.KICK, mid))
        elif name == EVT_BANNED:
            nick = nick_of(data.get("user") or {})
            if nick:
                self.event.emit(cs.purge(cs.KICK, nick))
        elif name == EVT_HOST:
            host = data.get("host_username") or ""
            if host:
                self.event.emit(cs.system(cs.KICK, "%s привів рейд: %s глядачів" % (
                    host, data.get("number_viewers", "?")), "raid"))
        elif name == EVT_PINNED:
            msg = data.get("message") or {}
            text, _ = extract_emotes(msg.get("content") or "")
            user = ((msg.get("sender") or {}).get("username")) or ""
            if text:
                self.event.emit(cs.system(cs.KICK, "Закріплено (%s): %s" % (user, text), "pin"))
        elif name == EVT_REWARD:
            user = data.get("username") or ""
            title = data.get("reward_title") or ""
            if user and title:
                self.event.emit(cs.system(cs.KICK, "%s витратив бали: %s" % (user, title), "points"))

    def _message(self, data: dict):
        sender = data.get("sender") or {}
        name = sender.get("username") or ""
        if not name:
            return
        text, emotes = extract_emotes((data.get("content") or "").strip())
        if not text:
            return
        identity = sender.get("identity") or {}
        meta = data.get("metadata") or {}
        # Сторонні емоути 7TV каналу (BTTV/FFZ каналів Kick не мають) + загальні.
        emotes = EMOTES.append(emotes, "kick", self.kick_id or "", text)
        norm = map_badges(identity)
        self.event.emit(cs.message(
            cs.KICK, nick_of(sender), name, text,
            id=data.get("id") or "",
            color=identity.get("color") or "",
            badges=norm,
            badge_icons=BADGE_ICONS.kick(norm),
            emotes=emotes,
            reply=((meta.get("original_sender") or {}).get("username") or ""),
        ))
