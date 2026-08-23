"""Де що лежить: ресурси всередині збірки і робочі файли поруч із нею."""

import os
import sys
import tempfile


def resource_path(name: str) -> str:
    """Шлях до ресурсу (працює і в .exe через PyInstaller _MEIPASS).

    Без збірки ресурси лежать поруч із точкою входу, тобто на рівень вище за
    цей файл: сам пакет — це код, а не картинки.
    """
    default = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(getattr(sys, "_MEIPASS", default), name)


# config.json — поряд з .exe (або зі скриптом у dev-режимі), щоб налаштування
# зберігались і в зібраній програмі.
if getattr(sys, "frozen", False):
    BASE_DIR = os.path.dirname(sys.executable)
else:
    # У пакеті цей файл лежить на рівень глибше, ніж точка входу, — тому два
    # dirname: налаштування мусять лежати поруч із chat_overlay.py, а не
    # всередині hominka/.
    BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_PATH = os.path.join(BASE_DIR, "config.json")


def profile_dir() -> str:
    """Тека профілю браузера: кеш сторінок YouTube і його власні куки згоди.

    Поряд із програмою — щоб копію можна було перенести цілком; оновлення її не
    чіпає (підмінник копіює нове ПОВЕРХ старого). Якщо туди не пишеться
    (розпакували в Program Files), відступаємо в LOCALAPPDATA: інакше кожен
    запуск заново тягнув би кілька мегабайт скриптів YouTube.
    """
    here = os.path.join(BASE_DIR, "profile")
    try:
        os.makedirs(here, exist_ok=True)
        probe = os.path.join(here, ".writable")
        with open(probe, "w") as f:
            f.write("1")
        os.remove(probe)
        return here
    except OSError:
        alt = os.path.join(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir(),
                           "Hominka", "profile")
        os.makedirs(alt, exist_ok=True)
        return alt
