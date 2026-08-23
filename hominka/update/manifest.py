"""Маніфест випуску: розбір, вибір файлу під систему, перевірка підпису.

Підпис перевіряється саме тут — до того, як хтось встигне щось завантажити:
відмовити треба на етапі «що нам пропонують», а не «що ми вже скачали».
"""

import json
import sys
from dataclasses import dataclass, field

from .. import signing
from .channels import channel_label, kind_label

# Базова адреса оновлень. Змінюється разом із доменом, тому окремою константою.
UPDATE_BASE = "https://update.svitix.com/hominka/"


# Публічні ключі, чиїм підписам віримо (base64, Ed25519).
#
# Приватна половина — у того, хто випускає, і на сервері її немає ніколи.
# Список, а не один ключ: так можна ввести новий ключ випуском, який знає обидва,
# і лише потім прибрати старий.
RELEASE_KEYS = [
    "Hj9RXkr1ACooavzVQ3qbqhZ+FaqgsnGgoRM5ItMYcPo=",
]


# Для якої системи шукати файл у маніфесті. Випуск один, файлів у ньому може
# бути кілька: Windows і Linux оновлюються з того самого каналу, але качати
# мусять різні архіви.
PLATFORM = "win64" if sys.platform == "win32" else "linux64"


@dataclass
class Release:
    channel: str = ""
    version: str = ""
    kind: str = ""
    notes: str = ""
    warning: str = ""
    released_at: str = ""
    mandatory: bool = False
    url: str = ""
    sha256: str = ""
    size: int = 0
    history: list = field(default_factory=list)

    @property
    def title(self) -> str:
        return "%s %s (%s)" % (channel_label(self.channel), self.version, kind_label(self.kind))


def file_for_platform(data: dict) -> dict:
    """Файл випуску для цієї системи.

    Старі маніфести знають лише "file" (там завжди був Windows) — їх читаємо
    як і раніше, інакше вже встановлені програми перестали б оновлюватися.
    Нові додають "files": список по одному запису на систему.
    """
    for f in data.get("files") or []:
        if (f or {}).get("platform") == PLATFORM:
            return f
    f = data.get("file") or {}
    if f and f.get("platform", "win64") == PLATFORM:
        return f
    return {}


def parse_manifest(raw: bytes, channel: str) -> Release:
    data = json.loads(raw.decode("utf-8"))
    f = file_for_platform(data)
    if not f:
        raise ValueError("для цієї системи (%s) випуску немає" % PLATFORM)
    rel = Release(
        channel=data.get("channel") or channel,
        version=str(data.get("version") or ""),
        kind=data.get("kind") or "",
        notes=data.get("notes") or "",
        warning=data.get("warning") or "",
        released_at=data.get("releasedAt") or "",
        mandatory=bool(data.get("mandatory")),
        url=f.get("url") or "",
        sha256=(f.get("sha256") or "").lower(),
        size=int(f.get("size") or 0),
        history=data.get("history") or [],
    )
    if not rel.version or not rel.url:
        raise ValueError("маніфест без версії або без файлу")
    verify_signature(data)
    # Завантажувати будемо тільки з нашого домену: маніфест теж приїхав із
    # мережі, і посилання «кудись іще» — привід зупинитися, а не качати.
    if not rel.url.startswith(UPDATE_BASE):
        raise ValueError("посилання на файл поза %s" % UPDATE_BASE)
    return rel


def verify_signature(data: dict):
    """Пропускає лише те, що підписано нашим ключем.

    Кидає ValueError — і це навмисно жорстко: сумнівне оновлення краще не
    поставити зовсім, ніж поставити «про всяк випадок». Текст помилки видно в
    налаштуваннях, тож мовчазного провалу не буде.
    """
    sig = (data.get("signature") or "").strip()
    if not sig:
        raise ValueError("випуск без підпису — не встановлюємо")
    try:
        raw = signing.unb64(sig)
    except Exception:
        raise ValueError("підпис випуску пошкоджено")
    payload = signing.release_payload(data)
    for key in RELEASE_KEYS:
        if signing.verify(signing.unb64(key), raw, payload):
            return
    raise ValueError("підпис випуску не сходиться — файл або маніфест підмінено")
