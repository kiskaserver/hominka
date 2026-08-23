"""Опис оновлення для панелі: версія зверху, зміни списком."""

import re

from . import update as updater


def update_status_html(rel) -> str:
    """Опис оновлення для ⚙ — рядками, а не суцільною стрічкою.

    Раніше це був один довгий рядок «Доступно: Стабільна 1.8.0 (нові
    можливості). Що нового: …» — його доводилося дочитувати, щоб зрозуміти
    навіть номер версії. Тепер зверху версія, під нею список змін.
    """
    head = "Доступно <b>%s</b> · %s · %s" % (
        esc(rel.version), esc(updater.channel_label(rel.channel)), esc(updater.kind_label(rel.kind)))
    warning = " ".join((getattr(rel, "warning", "") or "").split())
    if warning:
        head += ("<br><span style='color:#fca5a5'>Увага: %s</span>" % esc(warning))
    items = split_notes(rel.notes)
    if not items:
        return head
    body = "".join("<br>• %s" % esc(i) for i in items)
    return head + "<br><span style='color:#8f8a86'>Що нового:</span>" + body


def split_notes(notes: str) -> list:
    """Розбиває опис змін на пункти.

    Опис пишеться однією фразою, але майже завжди складається з кількох
    речень — саме їх і показуємо окремими рядками, інакше в панелі суцільна
    стіна тексту.
    """
    text = " ".join((notes or "").split())
    if not text:
        return []
    parts = re.split(r"(?<=[.!?;])\s+", text)
    return [p.strip(" ;") for p in parts if p.strip(" ;")]


def esc(text: str) -> str:
    return (str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))
