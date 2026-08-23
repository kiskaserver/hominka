"""Канали оновлень і порівняння версій.

Канал — це просто окремий маніфест на сервері, а не окрема програма: людина в
налаштуваннях обирає, з якого читати.
"""

import re

# Канали оновлень. Порядок = від найспокійнішого до найсвіжішого.
CHANNELS = [
    ("stable", "Стабільна", "Перевірені випуски. Рекомендовано."),
    ("beta", "Бета", "Свіжі можливості до того, як вони потраплять у стабільну."),
    ("dev", "Тестова", "Збірки одразу після змін. Можуть ламатися."),
]


DEFAULT_CHANNEL = "stable"


# Як показувати вид оновлення.
KIND_LABELS = {
    "major": "велике оновлення",
    "minor": "нові можливості",
    "patch": "виправлення",
    "hotfix": "термінове виправлення",
}


def channel_label(channel: str) -> str:
    for cid, label, _ in CHANNELS:
        if cid == channel:
            return label
    return channel


def kind_label(kind: str) -> str:
    return KIND_LABELS.get(kind, kind or "оновлення")


# --- версії ----------------------------------------------------------------


def parse_version(s: str) -> tuple:
    """«1.10.2» → (1, 10, 2). Нецифрові хвости («1.2.0-beta») відкидаємо:
    канал і так відомий, а порівнювати треба саме числа."""
    nums = re.findall(r"\d+", (s or "").split("+")[0])
    parts = [int(n) for n in nums[:3]]
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts)


def is_newer(candidate: str, current: str) -> bool:
    return parse_version(candidate) > parse_version(current)


# --- випуск ----------------------------------------------------------------
