"""Де що лежить — одне місце на всі перевірки.

Кожен скрипт раніше рахував шлях до кореня сам, ланцюжком із трьох-чотирьох
dirname. Читати таке важко, а варто було посунути файл на теку глибше — і
ламалися вони всі одразу й мовчки.

Знімки екрана пишемо в тимчасову теку, а не поруч із кодом: це артефакти
перевірки, у сховищі їм не місце (і саме так вони туди колись і потрапили —
п'ять PNG на пів мегабайта).
"""

import os
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
RENDER = os.path.dirname(TOOLS)
NATIVE = os.path.dirname(RENDER)
APP = os.path.dirname(NATIVE)                     # тека chat-overlay
DIST = os.path.join(NATIVE, "dist")

EXE = os.path.join(DIST, "hominka-render-x64.exe")
EXE_LINUX = os.path.join(DIST, "hominka-render-linux")

SHOTS = os.path.join(tempfile.gettempdir(), "hominka-shots")


def shot_path(name: str) -> str:
    """Шлях для знімка перевірки. Теку створює сам."""
    os.makedirs(SHOTS, exist_ok=True)
    return os.path.join(SHOTS, name)
