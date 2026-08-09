"""
Читання чату YouTube — через InnerTube, той самий внутрішній API, яким
користується сама сторінка YouTube. Ключів не треба, добової квоти в нього
немає (на відміну від YouTube Data API).

Порт серверного читача (server/internal/youtubechat). Три речі, на яких там уже
спіткалися, повторені й тут:

  • беремо режим «Live chat», а не «Top chat»: у другому YouTube ховає частину
    повідомлень, і зритель просто не з'являється в чаті;
  • токен цього режиму треба брати зі сторінки поп-ауту чату — на сторінці
    watch він обрізаний, і get_live_chat відповідає на нього 400;
  • суперчат це донат: сума важливіша за текст, а стикер узагалі буває без
    тексту, і викидати «порожні» повідомлення означає губити гроші.

Мережа крутиться в окремому потоці, назовні йдуть сигнали Qt.
"""

import json
import re
import time
import urllib.request
from threading import Event, Thread

from PySide6.QtCore import QObject, Signal

import chatsources as cs

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36")
HEADERS = {"User-Agent": UA, "Accept-Language": "en-US,en;q=0.9",
           "Cookie": "CONSENT=YES+cb; PREF=hl=en"}
FALLBACK_KEY = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8"
FALLBACK_VER = "2.20240726.00.00"

RE_KEY = re.compile(r'"INNERTUBE_API_KEY":"([^"]+)"')
RE_VER = re.compile(r'"INNERTUBE_CLIENT_VERSION":"([^"]+)"')
RE_VER2 = re.compile(r'"clientVersion":"(2\.[\d.]+)"')

POLL_MIN = 1.0
POLL_DEFAULT = 2.0
RETRY = 6.0
LIVE_RECHECK = 60.0


def _get(url: str) -> str:
    req = urllib.request.Request(url, headers=HEADERS)
    return urllib.request.urlopen(req, timeout=20).read().decode("utf-8", "replace")


def extract_json(html: str, marker: str):
    """Дістає `<marker> = {...}` зі сторінки: рахуємо дужки, бо після об'єкта
    в тому ж рядку ще купа коду."""
    for pat in (marker + " = ", marker + '"] = '):
        i = html.find(pat)
        if i < 0:
            continue
        s = html[i + len(pat):]
        depth, in_str, esc = 0, False, False
        for n, ch in enumerate(s):
            if in_str:
                if esc:
                    esc = False
                elif ch == "\\":
                    esc = True
                elif ch == '"':
                    in_str = False
                continue
            if ch == '"':
                in_str = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    try:
                        return json.loads(s[:n + 1])
                    except ValueError:
                        return None
        return None
    return None


def jget(obj, *keys):
    cur = obj
    for k in keys:
        if not isinstance(cur, dict):
            return None
        cur = cur.get(k)
    return cur


def runs_to_text(runs):
    """Текст + картинки кастомних емодзі каналу."""
    text, emotes = "", []
    for r in runs or []:
        if not isinstance(r, dict):
            continue
        if r.get("text"):
            text += r["text"]
            continue
        em = r.get("emoji") or {}
        if not em:
            continue
        if em.get("emojiId") and not em.get("isCustomEmoji"):
            text += em["emojiId"]          # звичайний emoji — це сам символ
            continue
        shortcuts = em.get("shortcuts") or []
        if not shortcuts:
            continue
        code = shortcuts[0]
        text += code
        thumbs = jget(em, "image", "thumbnails") or []
        best = max(thumbs, key=lambda t: t.get("width", 0), default=None)
        if best and best.get("url"):
            emotes.append({"code": code, "url": best["url"]})
    return text, emotes


def author_badges(items):
    """Значки автора → спільний набір."""
    out = []
    for b in items or []:
        r = jget(b, "liveChatAuthorBadgeRenderer") or {}
        icon = jget(r, "icon", "iconType")
        if icon == "OWNER":
            out.append("broadcaster")
        elif icon == "MODERATOR":
            out.append("mod")
        elif icon == "VERIFIED":
            out.append("verified")
        elif "customThumbnail" in r:
            # Саме наявність ключа: у учасника каналу замість іконки картинка
            # рівня членства, і порожній словник тут — теж «так, учасник».
            out.append("member")
    return out


def find_live_video(channel: str) -> str:
    """Канал (@нік або UC-id) → id трансляції, що ЗАРАЗ в ефірі."""
    if channel.startswith("UC") and len(channel) == 24:
        url = "https://www.youtube.com/channel/%s/live?hl=en" % channel
    elif channel.startswith("@"):
        url = "https://www.youtube.com/%s/live?hl=en" % channel
    else:
        m = re.search(r"(?:v=|youtu\.be/|/live/)([A-Za-z0-9_-]{11})", channel)
        if m:
            return m.group(1)
        url = "https://www.youtube.com/@%s/live?hl=en" % channel.lstrip("@")
    data = extract_json(_get(url), "ytInitialData")
    if not data:
        return ""
    vid = jget(data, "currentVideoEndpoint", "watchEndpoint", "videoId") or ""
    if not vid:
        return ""
    # Сторінка перегляду буває і в завершеного ефіру, і в анонса — беремо лише
    # той, що ЗАРАЗ в ефірі.
    #
    # separators обов'язкові: json.dumps за замовчуванням ставить пробіл після
    # двокрапки, і шукати в такому рядку «"isLive":true» — значить не знайти
    # ніколи (перевірка мовчки казала «ефіру немає» навіть посеред ефіру).
    compact = json.dumps(data, separators=(",", ":"))
    return vid if '"isLive":true' in compact else ""


def open_session(video_id: str):
    """(ключ, версія клієнта, continuation режиму «Live chat»)."""
    html = _get("https://www.youtube.com/live_chat?v=%s&is_popout=1&hl=en" % video_id)
    key = (RE_KEY.search(html).group(1) if RE_KEY.search(html) else FALLBACK_KEY)
    ver = RE_VER.search(html) or RE_VER2.search(html)
    ver = ver.group(1) if ver else FALLBACK_VER
    data = extract_json(html, "ytInitialData")
    lcr = jget(data, "contents", "liveChatRenderer")
    if not lcr:
        return "", "", ""
    cont = ""
    items = jget(lcr, "header", "liveChatHeaderRenderer", "viewSelector",
                 "sortFilterSubMenuRenderer", "subMenuItems") or []
    # Пункти йдуть [Цікавий чат, Чат наживо]; беремо останній невибраний.
    for it in reversed(items):
        if it.get("selected"):
            continue
        c = jget(it, "continuation", "reloadContinuationData", "continuation")
        if c:
            cont = c
            break
    if not cont:
        for shape in ("invalidationContinuationData", "timedContinuationData",
                      "reloadContinuationData"):
            c = jget((lcr.get("continuations") or [{}])[0], shape, "continuation")
            if c:
                cont = c
                break
    return key, ver, cont


def poll(key: str, ver: str, cont: str):
    """Один запит get_live_chat → (події, наступний continuation, пауза)."""
    body = json.dumps({
        "context": {"client": {"clientName": "WEB", "clientVersion": ver, "hl": "en"}},
        "continuation": cont,
    }).encode()
    req = urllib.request.Request(
        "https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?key=%s&prettyPrint=false" % key,
        data=body, headers=dict(HEADERS, **{"Content-Type": "application/json"}))
    data = json.loads(urllib.request.urlopen(req, timeout=25).read())
    lc = jget(data, "continuationContents", "liveChatContinuation")
    if not lc:
        return [], "", POLL_DEFAULT       # чат завершено

    conts = (lc.get("continuations") or [{}])[0]
    nxt, timeout = "", 0
    for shape in ("invalidationContinuationData", "timedContinuationData",
                  "reloadContinuationData"):
        node = conts.get(shape) or {}
        if node.get("continuation"):
            nxt = node["continuation"]
            timeout = node.get("timeoutMs", 0) / 1000.0
            break
    return parse_actions(lc.get("actions") or []), nxt, max(timeout, POLL_MIN)


def parse_actions(actions):
    """Дії чату → спільні події (див. chatsources)."""
    out = []
    for a in actions:
        if not isinstance(a, dict):
            continue
        item = jget(a, "addChatItemAction", "item")
        if item:
            ev = _item(item)
            if ev:
                out.append(ev)
            continue
        mid = jget(a, "markChatItemAsDeletedAction", "targetItemId")
        if mid:
            out.append(cs.delete(cs.YOUTUBE, mid))
            continue
        # Бан автора приходить з id каналу; ніком у стрічці ми його не знаємо,
        # тому чистимо за тим самим ключем, яким підписуємо повідомлення.
        ch = jget(a, "markChatItemsByAuthorAsDeletedAction", "externalChannelId")
        if ch:
            out.append(cs.purge(cs.YOUTUBE, "yt:" + ch))
    return out


def _item(item):
    for key, amount_path in (("liveChatTextMessageRenderer", None),
                             ("liveChatPaidMessageRenderer", ("purchaseAmountText", "simpleText")),
                             ("liveChatPaidStickerRenderer", ("purchaseAmountText", "simpleText"))):
        r = item.get(key)
        if not r:
            continue
        text, emotes = runs_to_text(jget(r, "message", "runs"))
        amount = jget(r, *amount_path) if amount_path else ""
        if not text.strip() and not amount:
            return None
        name = (jget(r, "authorName", "simpleText") or "").lstrip("@")
        channel = r.get("authorExternalChannelId") or ""
        return cs.message(
            cs.YOUTUBE, "yt:" + channel if channel else name.lower(), name, text,
            id=r.get("id", ""), badges=author_badges(r.get("authorBadges")),
            emotes=emotes, amount=amount or "")

    r = item.get("liveChatMembershipItemRenderer")
    if r:
        user = (jget(r, "authorName", "simpleText") or "").lstrip("@")
        head, _ = runs_to_text(jget(r, "headerPrimaryText", "runs"))
        sub = jget(r, "headerSubtext", "simpleText") or ""
        return cs.system(cs.YOUTUBE, ("%s — %s" % (user, head or sub)).strip(" —"))

    r = item.get("liveChatSponsorshipsGiftPurchaseAnnouncementRenderer")
    if r:
        hdr = jget(r, "header", "liveChatSponsorshipsHeaderRenderer") or {}
        user = (jget(hdr, "authorName", "simpleText") or "").lstrip("@")
        text, _ = runs_to_text(jget(hdr, "primaryText", "runs"))
        return cs.system(cs.YOUTUBE, "%s — %s" % (user, text) if text else user)

    r = item.get("liveChatSponsorshipsGiftRedemptionAnnouncementRenderer")
    if r:
        text, _ = runs_to_text(jget(r, "message", "runs"))
        user = (jget(r, "authorName", "simpleText") or "").lstrip("@")
        return cs.system(cs.YOUTUBE, ("%s %s" % (user, text)).strip())

    r = item.get("liveChatModeChangeMessageRenderer")
    if r:
        text, _ = runs_to_text(jget(r, "text", "runs"))
        return cs.system(cs.YOUTUBE, text) if text else None
    return None


class YouTubeChat(QObject):
    """Читає чат каналу YouTube. Сам знаходить трансляцію і чекає її початку."""

    event = Signal(dict)
    status = Signal(str)

    def __init__(self, channel: str, parent=None):
        super().__init__(parent)
        self.channel = (channel or "").strip()
        self._stop = Event()
        self._thread = None

    def start(self):
        if self._thread or not self.channel:
            return
        self._thread = Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()

    def _run(self):
        while not self._stop.is_set():
            try:
                video = find_live_video(self.channel)
            except Exception as e:
                self.status.emit("YouTube: %s" % e)
                video = ""
            if not video:
                # Ефіру немає — це нормальний стан, а не помилка.
                self._stop.wait(LIVE_RECHECK)
                continue
            try:
                self._read(video)
            except Exception as e:
                self.status.emit("YouTube: %s" % e)
                self._stop.wait(RETRY)

    def _read(self, video: str):
        key, ver, cont = open_session(video)
        if not cont:
            self._stop.wait(RETRY)
            return
        self.status.emit("")
        first = True
        while not self._stop.is_set() and cont:
            events, cont, pause = poll(key, ver, cont)
            # Перша пачка — це історія чату; сипати нею в стрічку ні до чого.
            if not first:
                for e in events:
                    self.event.emit(e)
            first = False
            self._stop.wait(pause or POLL_DEFAULT)
