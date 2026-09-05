"""
Сторонні емоути 7TV / BTTV / FFZ у стрічці (feed-режим програми).

Те саме, що робить сервер для чату на сайті (server/internal/emotes), тільки
тут — прямо в програмі, для стрімерів, які читають Twitch/Kick/YouTube
безпосередньо через Hominka, а не через оверлей сайту.

Навіщо. Глядачі масово пишуть емоутами 7TV/BTTV/FFZ (catJAM, OMEGALUL, свої
канальні), а площадки в чаті віддають лише власні. Без цього половина чату —
незрозумілі слова там, де в усіх інших картинки.

Як. На канал (за числовим id площадки) один раз тягнемо набір каналу плюс
загальний набір, тримаємо в памʼяті й оновлюємо у фоні. До кожного повідомлення
додаємо ті емоути, чиї імена стоять у тексті ОКРЕМИМИ словами (пробіл по краях,
регістр важливий: catJAM ≠ catjam). Рідні емоути площадки мають пріоритет.

Завантаження не блокує читання чату: перші повідомлення йдуть без сторонніх
картинок, далі — з ними. Загальний набір діє навіть без id каналу (для
YouTube, де id ведучого не завжди під рукою).
"""

import json
import threading
import time
import urllib.request

_TTL_CHANNEL = 1800.0      # 30 хв — набори каналу міняються рідко
_TTL_GLOBAL = 21600.0      # 6 год
_RETRY = 90.0              # після помилки повторюємо не раніше ніж за
_TIMEOUT = 8
_UA = "Hominka-overlay/1.0 (+https://stream.svitix.com)"


def _get_json(url: str):
    """GET JSON або None на будь-якій заминці (мережа, 404, кривий JSON)."""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": _UA, "Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=_TIMEOUT) as r:
            return json.loads(r.read())
    except Exception:
        return None


class _Entry:
    """Один набір і його стан. emap після завантаження не міняємо, а замінюємо
    цілком — тож читати його можна без блокування, забравши посилання під lock."""

    __slots__ = ("lock", "emap", "loaded_at", "loading")

    def __init__(self):
        self.lock = threading.Lock()
        self.emap = None
        self.loaded_at = 0.0
        self.loading = False


class ThirdParty:
    """Провайдер сторонніх емоутів. Потокобезпечний; один на весь застосунок."""

    def __init__(self):
        self._global = _Entry()
        self._chans = {}
        self._chans_lock = threading.Lock()

    def append(self, native, platform, channel_id, text):
        """Дописує до рідних емоутів знайдені в тексті сторонні. Рідні (native)
        мають пріоритет: код, який там уже є, не задвоюється. Загальний набір
        діє завжди, канальний — лише коли є channel_id."""
        if not text:
            return native
        g = self._ensure(self._global, _TTL_GLOBAL, self._fetch_global)
        ch = self._channel(platform, str(channel_id)) if channel_id else None
        if not g and not ch:
            return native
        seen = set()
        for e in native:
            code = e.get("code")
            if code:
                seen.add(code)
        out = None
        for tok in text.split():          # split() ріже по будь-яких пробілах
            if tok in seen:
                continue
            em = (ch.get(tok) if ch else None) or (g.get(tok) if g else None)
            if not em:
                continue
            seen.add(tok)
            if out is None:
                out = list(native)
            out.append(em)
        return out if out is not None else native

    # ── кеш ──────────────────────────────────────────────────────────────────

    def _channel(self, platform, cid):
        key = platform + ":" + cid
        with self._chans_lock:
            e = self._chans.get(key)
            if e is None:
                e = _Entry()
                self._chans[key] = e
        return self._ensure(e, _TTL_CHANNEL, lambda: self._fetch_channel(platform, cid))

    def _ensure(self, e, ttl, fetch):
        with e.lock:
            cur = e.emap
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
                e.emap = m
                e.loaded_at = time.monotonic()
            else:                          # помилка: дозволяємо повтор за _RETRY
                e.loaded_at = time.monotonic() - ttl + _RETRY
            e.loading = False

    # ── збірка наборів ────────────────────────────────────────────────────────
    # Порядок update() важливий: пізніший перебиває раніший, тож 7TV кличемо
    # останнім — при збігу імені перемагає він.

    def _fetch_global(self):
        out = {}
        for m in (self._ffz_global(), self._bttv_global(), self._seventv_global()):
            if m:
                out.update(m)
        return out or None

    def _fetch_channel(self, platform, cid):
        out = {}
        if platform == "twitch":
            for m in (self._ffz_room(cid), self._bttv_user("twitch", cid),
                      self._seventv_user("twitch", cid)):
                if m:
                    out.update(m)
        elif platform == "kick":
            m = self._seventv_user("kick", cid)   # BTTV/FFZ каналів Kick не мають
            if m:
                out.update(m)
        elif platform == "youtube":
            for m in (self._bttv_user("youtube", cid), self._seventv_user("youtube", cid)):
                if m:
                    out.update(m)
        return out

    # ── 7TV ────────────────────────────────────────────────────────────────────

    @staticmethod
    def _seventv_url(eid):
        return "https://cdn.7tv.app/emote/%s/2x.webp" % eid   # webp: мале + анімація

    def _seventv_map(self, lst):
        out = {}
        for e in lst or []:
            name, eid = e.get("name"), e.get("id")
            if name and eid:
                out[name] = {"code": name, "url": self._seventv_url(eid)}
        return out

    def _seventv_global(self):
        d = _get_json("https://7tv.io/v3/emote-sets/global")
        return self._seventv_map(d.get("emotes")) if isinstance(d, dict) else None

    def _seventv_user(self, platform, cid):
        d = _get_json("https://7tv.io/v3/users/%s/%s" % (platform, cid))
        if not isinstance(d, dict):
            return None
        return self._seventv_map((d.get("emote_set") or {}).get("emotes"))

    # ── BetterTTV ────────────────────────────────────────────────────────────

    @staticmethod
    def _bttv_url(eid):
        return "https://cdn.betterttv.net/emote/%s/2x" % eid

    def _bttv_map(self, lst):
        out = {}
        for e in lst or []:
            code, eid = e.get("code"), e.get("id")
            if code and eid:
                out[code] = {"code": code, "url": self._bttv_url(eid)}
        return out

    def _bttv_global(self):
        d = _get_json("https://api.betterttv.net/3/cached/emotes/global")
        return self._bttv_map(d) if isinstance(d, list) else None

    def _bttv_user(self, platform, cid):
        d = _get_json("https://api.betterttv.net/3/cached/users/%s/%s" % (platform, cid))
        if not isinstance(d, dict):
            return None
        out = self._bttv_map(d.get("channelEmotes"))
        out.update(self._bttv_map(d.get("sharedEmotes")))
        return out

    # ── FrankerFaceZ ───────────────────────────────────────────────────────────

    @staticmethod
    def _ffz_pick(m):
        """Кращий доступний адрес: 2x, інакше 4x, інакше 1x; url бува без схеми."""
        if not m:
            return ""
        for s in ("2", "4", "1"):
            u = m.get(s)
            if not u:
                continue
            if u.startswith("//"):
                return "https:" + u
            if u.startswith("http"):
                return u
        return ""

    def _ffz_map(self, lst):
        out = {}
        for e in lst or []:
            name = e.get("name")
            if not name:
                continue
            u = self._ffz_pick(e.get("animated")) or self._ffz_pick(e.get("urls"))
            if u:
                out[name] = {"code": name, "url": u}
        return out

    def _ffz_global(self):
        d = _get_json("https://api.frankerfacez.com/v1/set/global")
        if not isinstance(d, dict):
            return None
        out, sets = {}, (d.get("sets") or {})
        for sid in d.get("default_sets") or []:
            out.update(self._ffz_map((sets.get(str(sid)) or {}).get("emoticons")))
        return out

    def _ffz_room(self, cid):
        d = _get_json("https://api.frankerfacez.com/v1/room/id/%s" % cid)
        if not isinstance(d, dict):
            return None
        sid = str((d.get("room") or {}).get("set") or "")
        return self._ffz_map(((d.get("sets") or {}).get(sid) or {}).get("emoticons"))


# Один провайдер на процес: кеш спільний для всіх читачів.
EMOTES = ThirdParty()
