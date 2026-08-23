"""JS, який ми вставляємо в чужу сторінку чату YouTube.

По файлу на кожну вставку: кожна з них — окремий обхід чужої поведінки, і
читати їх разом (три з половиною сотні рядків) було нічим не краще, ніж
шукати потрібну в одному довгому файлі.
"""

from .livechat import YT_ALL_MESSAGES_JS
from .mentions import YT_MENTIONS_JS
from .reactions import YT_REACTIONS_JS
from .style import YT_STYLE_JS

__all__ = ["YT_ALL_MESSAGES_JS", "YT_MENTIONS_JS", "YT_REACTIONS_JS", "YT_STYLE_JS"]


