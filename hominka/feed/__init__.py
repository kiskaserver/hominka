"""Спільна стрічка чату: сторінка та її наповнення.

Розділено надвоє свідомо: `page.py` — це верстка і JS, тобто те, що бачить
глядач; `feed.py` — черга, затримка і темп подачі, тобто те, ЯК повідомлення
на цю верстку потрапляють. Правлять їх у різні моменти й з різних причин.
"""

from .feed import PACE_MS, ChatFeed
from .page import BADGE_LABELS, ICONS, PAGE, apply_css_js, page_html

__all__ = ["ChatFeed", "PACE_MS", "PAGE", "ICONS", "BADGE_LABELS",
           "page_html", "apply_css_js"]


