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

# Що саме сталося. Площадки називають це по-різному — «resub», «subscription»,
# «liveChatMembershipItemRenderer», — але для глядача це одне й те саме, і
# оформлювати він хоче саме подію, а не назву з чужого API. Порожній рядок —
# звичайне повідомлення.
EVENTS = (
    ("raid", "Рейд"),
    ("sub", "Підписка"),
    ("gift", "Подарована підписка"),
    ("announce", "Оголошення"),
    ("pin", "Закріплене"),
    ("points", "Бали каналу"),
    ("mode", "Зміна режиму чату"),
    ("bits", "Біти Twitch"),
    ("superchat", "Super Chat"),
)

EVENT_IDS = tuple(e for e, _label in EVENTS)


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


def trim_reply_mention(text: str, reply_to: str) -> str:
    """Прибирає «@адресат» на початку тексту відповіді.

    Twitch дописує звертання в саме повідомлення, Kick — як вийде. Позначку
    «кому відповідають» ми малюємо окремо (↳ нік), тож без цієї чистки ім'я
    стоїть у рядку двічі. Правило те саме, що й у чаті на сайті: ріжемо лише
    точний збіг і лише на початку — «@Саша, ти не правий» посеред фрази це вже
    слова автора, а не службова приписка площадки.
    """
    if not text or not reply_to or not text.startswith("@"):
        return text
    rest = text[1:]
    if not rest.lower().startswith(reply_to.lower()):
        return text
    rest = rest[len(reply_to):]
    rest = rest.lstrip(",:")
    trimmed = rest.strip()
    # Пробіл (або кінець рядка) після імені обов'язковий: інакше «@Сашко»
    # приймуть за відповідь «@Саша» і відріжуть шматок чужого ніка.
    if rest and trimmed == rest:
        return text
    return trimmed


def message(platform: str, nick: str, name: str, text: str, **extra) -> dict:
    """Повідомлення у спільному вигляді.

    Порожні поля не викидаємо навмисне: сторінка-приймач читає їх без перевірок,
    і один відсутній ключ там перетворився б на «undefined» посеред рядка.
    """
    reply = extra.get("reply", "")
    msg = {
        "kind": "msg",
        "platform": platform,
        "id": extra.get("id", ""),
        "nick": nick,
        "name": name or nick,
        "text": trim_reply_mention(text, reply),
        "color": extra.get("color", ""),
        "badges": [b for b in extra.get("badges", []) if b in BADGES],
        # Справжні картинки значків [{id,url}] у пару до нормалізованих badges:
        # де є іконка, сторінка малює її, де немає — прежню текстову плашку.
        "badgeIcons": extra.get("badge_icons", []),
        "emotes": extra.get("emotes", []),
        "reply": reply,
        "amount": extra.get("amount", ""),
        # Біти, Super Chat, донат — теж події, просто написані самим глядачем.
        "event": extra.get("event", "") if extra.get("event", "") in EVENT_IDS else "",
    }
    return msg


def system(platform: str, text: str, event: str = "") -> dict:
    """Подія площадки, яка не є повідомленням: підписка, рейд, оголошення.

    `event` — що саме сталося (див. EVENTS). Раніше всі вони приходили одним
    «системним» рядком, і відрізнити рейд від підписки в оформленні не було
    чим — тільки читаючи текст, тобто ніяк.
    """
    return {"kind": "system", "platform": platform, "text": text,
            "event": event if event in EVENT_IDS else ""}


def delete(platform: str, msg_id: str) -> dict:
    """Модератор видалив повідомлення на площадці."""
    return {"kind": "delete", "platform": platform, "id": msg_id}


def purge(platform: str, nick: str) -> dict:
    """Автора забанили: прибираємо все, що він написав."""
    return {"kind": "purge", "platform": platform, "nick": nick}
