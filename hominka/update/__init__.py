"""Оновлення Hominka з update.svitix.com.

Як це влаштовано
----------------
На сервері лежить по одному маніфесту на КАНАЛ оновлень:

    https://update.svitix.com/hominka/stable.json
    https://update.svitix.com/hominka/beta.json
    https://update.svitix.com/hominka/dev.json

Маніфест описує один — поточний для каналу — випуск:

    {
      "product": "hominka",
      "channel": "stable",
      "version": "1.1.0",
      "kind": "minor",              major | minor | patch | hotfix
      "mandatory": false,           критичне виправлення: пропонуємо наполегливо
      "releasedAt": "2026-08-08",
      "notes": "що змінилось",
      "file": {
        "url": "https://update.svitix.com/hominka/files/Hominka-1.1.0-win64.zip",
        "size": 219938349,
        "sha256": "…"
      },
      "history": [ {"version": "...", "kind": "...", "notes": "..."} ]
    }

Канали — це три різні маніфести, а не три різні програми: користувач у
налаштуваннях обирає, з якого читати. Тому «перевести на бету» і «повернути на
стабільну» — це просто зміна одного поля в config.json.

Чому саме так, а не «просто перевірити версію»:

  * версія в маніфесті, а не імена файлів у теці — сервер може віддавати
    будь-які імена, а програмі не треба вгадувати, що новіше;
  * kind (яке саме оновлення) показуємо користувачу: хотфікс і мажорне
    оновлення вимагають різного ставлення;
  * sha256 перевіряємо ПІСЛЯ завантаження — з мережі приїжджає код, який
    виконуватиметься на машині користувача, і обірваний або підмінений архів
    розпаковувати не можна;
  * розпаковуємо поруч і підміняємо теку окремим процесом: на Windows
    програма не може перезаписати власний .exe, поки він запущений.

Підпис випуску (з 2.0.0 — обов'язковий)
---------------------------------------
Раніше цілісність трималася на HTTPS до update.svitix.com і на sha256 у
маніфесті, що приїхав звідти ж, — тобто на ОДНОМУ джерелі. Хто отримає доступ
до цього домену, той підмінить і файл, і його контрольну суму, і програма
слухняно поставить чужий код на чужу машину.

Тепер кожен маніфест несе поле "signature": Ed25519-підпис від приватного
ключа, який лежить у того, хто випускає, і ніколи не буває на сервері.
Перевіряється він публічним ключем, зашитим у програму (RELEASE_KEYS). Немає
підпису або він не сходиться — оновлення НЕ ставиться, хоч би що віддав
сервер. Підписуємо не маніфест цілком, а суть випуску (див.
signing.release_payload): продукт, канал, версію і по кожному файлу — систему,
адресу, розмір і sha256. Історія та дата в підпис не входять: вони
переписуються при кожному наступному випуску каналу, і підпис ламався б ні
через що.

Ключів у списку може бути кілька — це шлях заміни скомпрометованого: спершу
випуск, який знає обидва ключі, потім перехід на новий."""

from .channels import (
    CHANNELS, DEFAULT_CHANNEL, KIND_LABELS, channel_label, is_newer, kind_label,
    parse_version,
)
from .download import DOWNLOAD_TIMEOUT, NET_TIMEOUT, USER_AGENT, Updater, cleanup_downloads
from .install import clean_env, install
from .manifest import (
    PLATFORM, RELEASE_KEYS, UPDATE_BASE, Release, file_for_platform, parse_manifest,
    verify_signature,
)

__all__ = [
    "CHANNELS", "DEFAULT_CHANNEL", "KIND_LABELS", "channel_label", "kind_label",
    "parse_version", "is_newer", "Release", "parse_manifest", "verify_signature",
    "file_for_platform", "PLATFORM", "UPDATE_BASE", "RELEASE_KEYS", "Updater",
    "cleanup_downloads", "install", "clean_env", "NET_TIMEOUT", "DOWNLOAD_TIMEOUT",
    "USER_AGENT",
]


