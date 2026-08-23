"""Знайти активний ефір каналу і відкрити сесію читання його чату."""

import json
import re
import urllib.request

from .net import (
    FALLBACK_KEY, FALLBACK_VER, HEADERS, RE_KEY, RE_VER, RE_VER2, _get,
    extract_json, jget,
)
from .parse import parse_actions


POLL_MIN = 1.0
POLL_DEFAULT = 2.0
RETRY = 6.0
LIVE_RECHECK = 60.0

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
