"""Звідки брати чат: розбір посилань і адрес каналів."""

import re
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit

from .version import UI_LANG


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


# === Розбір посилання на чат ================================================
def is_youtube(url: str) -> bool:
    return "youtube.com" in url or "youtu.be" in url


def popout_url(video_id: str) -> str:
    """Посилання на окреме вікно чату конкретної трансляції."""
    return f"https://www.youtube.com/live_chat?v={video_id}&is_popout=1"


def site_chat_url(raw: str) -> str:
    """Доводить вписане посилання на чат сайту до вигляду, придатного тут.

    Дописуємо два параметри, якщо їх немає:
      • lang — інакше сервер говорить англійською, і причини, з яких
        автомодерація прибрала повідомлення ("profanity", "harassment"), видно
        англійськими словами посеред українського вікна;
      • raw=1 — це ж особисте вікно стрімера, саме воно й показує вирізане.
    Те, що людина написала руками, не чіпаємо: свій lang сильніший за наш.
    """
    raw = (raw or "").strip()
    if not raw:
        return ""
    if "://" not in raw:
        raw = "https://" + raw
    try:
        u = urlsplit(raw)
    except ValueError:
        return raw
    q = dict(parse_qsl(u.query, keep_blank_values=True))
    q.setdefault("lang", UI_LANG)
    q.setdefault("raw", "1")
    return urlunsplit((u.scheme, u.netloc, u.path, urlencode(q), u.fragment))


def resolve_chat_url(raw: str, auto_video: str = "", site: str = "") -> str:
    """Куди дивитися вікну чату.

    Порядок навмисне такий:
      1. те, що людина вписала руками, — явний вибір сильніший за будь-яку
         автоматику (наприклад, читати чужу трансляцію);
      2. власна трансляція, знайдена після входу в YouTube, — головний режим:
         програма для того й є, щоб стрімер читав СВІЙ чат;
      3. чат сайту — коли входу немає або ефір не йде; порожньо, якщо
         посилання на нього ще не вписали.
    """
    raw = (raw or "").strip()
    if not raw:
        return popout_url(auto_video) if auto_video else site_chat_url(site)
    if "youtube.com/live_chat" in raw:
        return raw
    if is_youtube(raw):
        m = re.search(r"(?:v=|youtu\.be/|/live/|/embed/|/shorts/)([A-Za-z0-9_-]{11})", raw)
        if m:
            return popout_url(m.group(1))
    return raw
