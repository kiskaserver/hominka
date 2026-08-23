"""Сторінки YouTube: запит і витяг JSON, зашитого в HTML.

YouTube віддає стан сторінки не окремим API, а великим об'єктом усередині
розмітки. Тому «мережа» тут — наполовину сторінка, наполовину пошук дужок у тексті.
"""

import json
import re
import urllib.request

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36")
HEADERS = {"User-Agent": UA, "Accept-Language": "en-US,en;q=0.9",
           "Cookie": "CONSENT=YES+cb; PREF=hl=en"}
FALLBACK_KEY = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8"
FALLBACK_VER = "2.20240726.00.00"

RE_KEY = re.compile(r'"INNERTUBE_API_KEY":"([^"]+)"')
RE_VER = re.compile(r'"INNERTUBE_CLIENT_VERSION":"([^"]+)"')
RE_VER2 = re.compile(r'"clientVersion":"(2\.[\d.]+)"')


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
