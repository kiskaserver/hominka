"""Розбір відповіді чату: повідомлення, значки, гроші, видалення."""

from .. import chatsources as cs
from ..thirdparty import EMOTES
from .net import jget, runs_to_text

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


def author_badge_icons(items):
    """Справжні картинки значків. У YouTube картинка є лише у значка учасника
    (customThumbnail); власник/модератор/підтверджений приходять типом іконки
    без URL — для них лишається текстова плашка."""
    out = []
    for b in items or []:
        r = jget(b, "liveChatAuthorBadgeRenderer") or {}
        thumbs = jget(r, "customThumbnail", "thumbnails") or []
        if thumbs:
            url = (thumbs[-1] or {}).get("url") or ""
            if url:
                out.append({"id": "member", "url": url})
    return out


def parse_actions(actions, channel_id=""):
    """Дії чату → спільні події (див. chatsources). channel_id — UC-id ведучого
    для сторонніх канальних емоутів (може бути порожнім)."""
    out = []
    for a in actions:
        if not isinstance(a, dict):
            continue
        item = jget(a, "addChatItemAction", "item")
        if item:
            ev = _item(item, channel_id)
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


def _item(item, channel_id=""):
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
        # Рідні емодзі YouTube (runs) + сторонні 7TV/BTTV (загальні завжди,
        # канальні — коли відомий UC-id ведучого).
        emotes = EMOTES.append(emotes, "youtube", channel_id, text)
        return cs.message(
            cs.YOUTUBE, "yt:" + channel if channel else name.lower(), name, text,
            id=r.get("id", ""), badges=author_badges(r.get("authorBadges")),
            badge_icons=author_badge_icons(r.get("authorBadges")),
            emotes=emotes, amount=amount or "",
            event="superchat" if amount else "")

    r = item.get("liveChatMembershipItemRenderer")
    if r:
        user = (jget(r, "authorName", "simpleText") or "").lstrip("@")
        head, _ = runs_to_text(jget(r, "headerPrimaryText", "runs"))
        sub = jget(r, "headerSubtext", "simpleText") or ""
        # Спонсорство YouTube — та сама підписка, просто названа інакше.
        return cs.system(cs.YOUTUBE, ("%s — %s" % (user, head or sub)).strip(" —"), "sub")

    r = item.get("liveChatSponsorshipsGiftPurchaseAnnouncementRenderer")
    if r:
        hdr = jget(r, "header", "liveChatSponsorshipsHeaderRenderer") or {}
        user = (jget(hdr, "authorName", "simpleText") or "").lstrip("@")
        text, _ = runs_to_text(jget(hdr, "primaryText", "runs"))
        return cs.system(cs.YOUTUBE, "%s — %s" % (user, text) if text else user, "gift")

    r = item.get("liveChatSponsorshipsGiftRedemptionAnnouncementRenderer")
    if r:
        text, _ = runs_to_text(jget(r, "message", "runs"))
        user = (jget(r, "authorName", "simpleText") or "").lstrip("@")
        return cs.system(cs.YOUTUBE, ("%s %s" % (user, text)).strip(), "gift")

    r = item.get("liveChatModeChangeMessageRenderer")
    if r:
        text, _ = runs_to_text(jget(r, "text", "runs"))
        return cs.system(cs.YOUTUBE, text, "mode") if text else None
    return None
