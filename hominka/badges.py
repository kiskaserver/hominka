"""
Настоящие иконки значков автора (Twitch/Kick/YouTube) для стрічки — в пару до
нормалізованих текстових плашок. Де є картинка, малюємо її; де немає —
лишається прежня плашка.

Те саме робить сервер (server/internal/badges) для чату на сайті. Тут — у самій
програмі, для feed-режиму:
  • Twitch — відкритий ендпоінт badges.twitch.tv (глобальні + канальні набори,
    точні рівні підписки), кешуємо по каналах у фоні;
  • Kick — картинок у чаті немає, тож стандартні значки беремо з вбудованого
    набору (_KICK_BADGE_SVG, ті самі, що в сервера);
  • YouTube — картинка значка учасника приходить у самому повідомленні
    (customThumbnail), її дістає youtube/parse.py.
"""

import threading
import time

from .thirdparty import _get_json   # той самий GET-JSON з коротким таймаутом

_TTL_GLOBAL = 21600.0     # 6 год
_TTL_CHANNEL = 1800.0     # 30 хв
_RETRY = 90.0

# set_id значка Twitch → нормалізоване ім'я (як у chat_twitch.map_badges), щоб
# іконки збігалися з текстовими значками.
_NORM_TW = {
    "broadcaster": "broadcaster", "moderator": "mod", "vip": "vip",
    "subscriber": "sub", "founder": "sub", "partner": "verified",
    "staff": "staff", "admin": "staff", "global_mod": "staff",
    "artist-badge": "artist",
}

# Вбудовані іконки значків Kick — стилізовані впізнавані гліфи на скруглених
# плитках (не власна графіка Kick). Ті самі, що на сервері
# (server/internal/badges/kick.go), щоб feed і оверлей сайту виглядали однаково.
_KICK_BADGE_SVG = {
    "broadcaster": "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nIzUzZmMxOCcvPjxwYXRoIGQ9J00xMiA5LjVhMi41IDIuNSAwIDEgMCAwIDUgMi41IDIuNSAwIDAgMCAwLTV6bS01LjYtMy4xIDEuNCAxLjRhNS42IDUuNiAwIDAgMCAwIDguNGwtMS40IDEuNGE3LjYgNy42IDAgMCAxIDAtMTEuMnptMTEuMiAwYTcuNiA3LjYgMCAwIDEgMCAxMS4ybC0xLjQtMS40YTUuNiA1LjYgMCAwIDAgMC04LjR6TTQuNiA0IDYgNS40YTkuNiA5LjYgMCAwIDAgMCAxMy4yTDQuNiAyMGExMS42IDExLjYgMCAwIDEgMC0xNnptMTQuOCAwYTExLjYgMTEuNiAwIDAgMSAwIDE2TDE4IDE4LjZhOS42IDkuNiAwIDAgMCAwLTEzLjJ6JyBmaWxsPScjMDgyMTBhJy8+PC9zdmc+",
    "mod": "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nIzAwYzJhOCcvPjxwYXRoIGQ9J00xMiAyIDQgNXY2YzAgNSAzLjQgOC4zIDggOS42IDQuNi0xLjMgOC00LjYgOC05LjZWNXonIGZpbGw9JyNmZmYnLz48L3N2Zz4=",
    "vip": "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nI2MwNGJmZicvPjxwYXRoIGQ9J00zIDcuNSA3IDExbDUtNiA1IDYgNC0zLjUtMS44IDEwLjVINC44eicgZmlsbD0nI2ZmZicvPjwvc3ZnPg==",
    "sub": "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nI2ZmYjMxZicvPjxwYXRoIGQ9J00xMiAzLjVsMi42IDUuMyA1LjkuOS00LjMgNC4xIDEgNS44LTUuMi0yLjctNS4yIDIuNyAxLTUuOC00LjMtNC4xIDUuOS0uOXonIGZpbGw9JyMzYTI0MDAnLz48L3N2Zz4=",
    "verified": "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nIzFlOWJmMCcvPjxwYXRoIGQ9J00xMCAxNS40IDYuNCAxMS44IDUgMTMuMiAxMCAxOC4yIDE5IDkuMmwtMS40LTEuNHonIGZpbGw9JyNmZmYnLz48L3N2Zz4=",
    "staff": "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nIzhhOGY5OCcvPjxwYXRoIGQ9J00xMiA4LjVhMy41IDMuNSAwIDEgMCAwIDcgMy41IDMuNSAwIDAgMCAwLTd6bTktLjUtMi0uNmE3IDcgMCAwIDAtLjYtMS40bDEtMS44LTEuNC0xLjQtMS44IDFhNyA3IDAgMCAwLTEuNC0uNkwxNCAxaC0ybC0uNiAyYTcgNyAwIDAgMC0xLjQuNmwtMS44LTFMNi44IDRsMSAxLjhhNyA3IDAgMCAwLS42IDEuNEw1IDh2MmwyIC42YTcgNyAwIDAgMCAuNiAxLjRsLTEgMS44IDEuNCAxLjQgMS44LTFhNyA3IDAgMCAwIDEuNC42TDEyIDE5aDJsLjYtMmE3IDcgMCAwIDAgMS40LS42bDEuOCAxIDEuNC0xLjQtMS0xLjhhNyA3IDAgMCAwIC42LTEuNGwyLS42eicgZmlsbD0nI2ZmZicvPjwvc3ZnPg==",
    "og": "data:image/svg+xml;base64,PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHZpZXdCb3g9JzAgMCAyNCAyNCc+PHJlY3Qgd2lkdGg9JzI0JyBoZWlnaHQ9JzI0JyByeD0nNicgZmlsbD0nI2ZmN2EwMCcvPjx0ZXh0IHg9JzEyJyB5PScxNi41JyBmb250LWZhbWlseT0nU2Vnb2UgVUksQXJpYWwsc2Fucy1zZXJpZicgZm9udC1zaXplPScxMScgZm9udC13ZWlnaHQ9JzgwMCcgdGV4dC1hbmNob3I9J21pZGRsZScgZmlsbD0nI2ZmZic+T0c8L3RleHQ+PC9zdmc+",
}


class _Entry:
    __slots__ = ("lock", "m", "loaded_at", "loading")

    def __init__(self):
        self.lock = threading.Lock()
        self.m = None
        self.loaded_at = 0.0
        self.loading = False


class Icons:
    """Іконки значків. Потокобезпечний; один на застосунок. nil-подібний виклик
    не потрібен — методи самі повертають порожньо, поки набір не завантажено."""

    def __init__(self):
        self._global = _Entry()
        self._chans = {}
        self._lock = threading.Lock()

    def twitch(self, room_id, tag):
        """Сирий тег badges ('subscriber/12,moderator/1') + room-id каналу →
        список {id,url}. Канал перебиває глобальні; по одній іконці на id."""
        if not tag:
            return []
        g = self._ensure(self._global, _TTL_GLOBAL,
                         lambda: _fetch("https://badges.twitch.tv/v1/badges/global/display"))
        ch = self._channel(str(room_id)) if room_id else None
        if not g and not ch:
            return []
        out, seen = [], set()
        for part in tag.split(","):
            set_id, _, ver = part.partition("/")
            if not set_id:
                continue
            nid = _NORM_TW.get(set_id)
            if not nid or nid in seen:
                continue
            key = "%s/%s" % (set_id, ver)
            url = (ch.get(key) if ch else None) or (g.get(key) if g else None)
            if not url:
                continue
            seen.add(nid)
            out.append({"id": nid, "url": url})
        return out

    @staticmethod
    def kick(norm):
        """Нормалізовані значки Kick → вбудовані іконки (без мережі)."""
        out = []
        for nid in norm or []:
            url = _KICK_BADGE_SVG.get(nid)
            if url:
                out.append({"id": nid, "url": url})
        return out

    # ── кеш каналів Twitch ─────────────────────────────────────────────────────

    def _channel(self, room_id):
        with self._lock:
            e = self._chans.get(room_id)
            if e is None:
                e = _Entry()
                self._chans[room_id] = e
        return self._ensure(e, _TTL_CHANNEL, lambda: _fetch(
            "https://badges.twitch.tv/v1/badges/channels/%s/display" % room_id))

    def _ensure(self, e, ttl, fetch):
        with e.lock:
            cur = e.m
            stale = (time.monotonic() - e.loaded_at) > ttl
            if (cur is None or stale) and not e.loading:
                e.loading = True
                threading.Thread(target=self._load, args=(e, fetch, ttl), daemon=True).start()
        return cur

    def _load(self, e, fetch, ttl):
        try:
            m = fetch()
        except Exception:
            m = None
        with e.lock:
            if m is not None:
                e.m = m
                e.loaded_at = time.monotonic()
            else:
                e.loaded_at = time.monotonic() - ttl + _RETRY
            e.loading = False


def _fetch(url):
    """Документ badges.twitch.tv → {'set/version': url}. None на помилці."""
    d = _get_json(url)
    if not isinstance(d, dict):
        return None
    out = {}
    for set_id, s in (d.get("badge_sets") or {}).items():
        for ver, v in ((s or {}).get("versions") or {}).items():
            img = (v or {}).get("image_url_4x") or (v or {}).get("image_url_2x")
            if img:
                out["%s/%s" % (set_id, ver)] = img
    return out


# Один провайдер на процес.
ICONS = Icons()
