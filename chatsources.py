"""
Розбір посилань на канали й спільний вигляд повідомлення.

Програма читає чат із трьох площадок одночасно, тому їй потрібні дві спільні
речі: як з будь-якого посилання зрозуміти, що це за канал, і в якому вигляді
повідомлення з різних площадок кладуться в одну стрічку.

Нормалізація тут така сама, як на сервері (internal/hub): значки зводяться до
одного набору, бо малювати три різні комплекти позначок ні до чого — глядач
однаково розрізняє їх за іконкою площадки.
"""

import re

# Площадки, які вміємо читати.
YOUTUBE = "youtube"
TWITCH = "twitch"
KICK = "kick"

# Ті самі значки, що й на сайті: broadcaster | mod | vip | sub | member |
# verified | staff | og | artist.
BADGES = ("broadcaster", "mod", "vip", "sub", "member", "verified", "staff", "og", "artist")


def parse_source(text: str):
    """Що це за посилання. Повертає (площадка, канал) або (None, "").

    Канал — те, чим площадка сама його називає: логін у Twitch, slug у Kick,
    «@нік» або UC-id у YouTube. Приймаємо і голий нік: людина частіше копіює
    саме його, а не адресу.
    """
    raw = (text or "").strip()
    if not raw:
        return None, ""
    low = raw.lower()

    if "twitch.tv" in low:
        m = re.search(r"twitch\.tv/(?:popout/)?([A-Za-z0-9_]{3,25})", raw)
        return (TWITCH, m.group(1).lower()) if m else (None, "")
    if "kick.com" in low:
        m = re.search(r"kick\.com/([A-Za-z0-9_-]{2,32})", raw)
        return (KICK, m.group(1).lower()) if m else (None, "")
    if "youtube.com" in low or "youtu.be" in low:
        m = re.search(r"(UC[0-9A-Za-z_\-]{22})", raw)
        if m:
            return YOUTUBE, m.group(1)
        m = re.search(r"youtube\.com/@([A-Za-z0-9_.\-]{3,64})", raw)
        if m:
            return YOUTUBE, "@" + m.group(1)
        return YOUTUBE, raw           # watch?v=… — розбереться сам читач
    if raw.startswith("UC") and len(raw) == 24:
        return YOUTUBE, raw
    if raw.startswith("@"):
        return YOUTUBE, raw
    return None, ""


def message(platform: str, nick: str, name: str, text: str, **extra) -> dict:
    """Повідомлення у спільному вигляді.

    Порожні поля не викидаємо навмисне: сторінка-приймач читає їх без перевірок,
    і один відсутній ключ там перетворився б на «undefined» посеред рядка.
    """
    msg = {
        "kind": "msg",
        "platform": platform,
        "id": extra.get("id", ""),
        "nick": nick,
        "name": name or nick,
        "text": text,
        "color": extra.get("color", ""),
        "badges": [b for b in extra.get("badges", []) if b in BADGES],
        "emotes": extra.get("emotes", []),
        "reply": extra.get("reply", ""),
        "amount": extra.get("amount", ""),
    }
    return msg


def system(platform: str, text: str) -> dict:
    """Подія площадки, яка не є повідомленням: підписка, рейд, оголошення."""
    return {"kind": "system", "platform": platform, "text": text}


def delete(platform: str, msg_id: str) -> dict:
    """Модератор видалив повідомлення на площадці."""
    return {"kind": "delete", "platform": platform, "id": msg_id}


def purge(platform: str, nick: str) -> dict:
    """Автора забанили: прибираємо все, що він написав."""
    return {"kind": "purge", "platform": platform, "nick": nick}
