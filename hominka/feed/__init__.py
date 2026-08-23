"""Спільна стрічка чату: сторінка та її наповнення.

Розділено надвоє свідомо: `page.py` — це верстка і JS, тобто те, що бачить
глядач; `feed.py` — черга, затримка і темп подачі, тобто те, ЯК повідомлення
на цю верстку потрапляють. Правлять їх у різні моменти й з різних причин.
"""

from .feed import PACE_MS, ChatFeed
from .page import (
    BADGE_LABELS, DEFAULT_LAYOUT, ICONS, PAGE, PARTS, apply_css_js,
    apply_layout_js, clean_layout, page_html,
)

__all__ = ["ChatFeed", "PACE_MS", "PAGE", "ICONS", "BADGE_LABELS", "PARTS",
           "DEFAULT_LAYOUT", "page_html", "apply_css_js", "apply_layout_js",
           "clean_layout"]


