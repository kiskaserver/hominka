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


# BASE_DIR — де лежить САМА програма (.exe). Потрібне для native/ (інжектор),
# оновлення тощо — це шлях до встановленої програми, а не до її даних.
if getattr(sys, "frozen", False):
    BASE_DIR = os.path.dirname(sys.executable)
else:
    # У пакеті цей файл лежить на рівень глибше, ніж точка входу, — тому два
    # dirname: точка входу chat_overlay.py на рівень вище за hominka/.
    BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _data_dir() -> str:
    """Тека ДАНИХ програми: config.json і профіль браузера.

    У зібраній програмі — у %LOCALAPPDATA%\\Hominka: це стандартне місце для
    даних застосунку, воно НЕ приховане (шукати налаштування у прихованій теці
    поруч із .exe — знущання), і оновлення програми його не чіпає. Раніше все
    лежало поруч із .exe, і теку профілю доводилося ховати; тепер не треба.
    У dev-режимі — поруч зі скриптом, щоб не смітити в системі під час розробки.
    """
    if getattr(sys, "frozen", False):
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        d = os.path.join(base, "Hominka")
    else:
        d = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        os.makedirs(d, exist_ok=True)
    except OSError:
        pass
    return d


DATA_DIR = _data_dir()
CONFIG_PATH = os.path.join(DATA_DIR, "config.json")


def _migrate_old_config():
    """Старіші версії тримали config.json ПОРУЧ із .exe. Якщо в новому місці
    (AppData) його ще нема, а старий є — переносимо копією, щоб не втратити
    налаштування користувача при оновленні. Копіюємо (не рухаємо): якщо щось
    піде не так, старий лишиться недоторканим."""
    if not getattr(sys, "frozen", False):
        return
    old = os.path.join(BASE_DIR, "config.json")
    if os.path.isfile(old) and not os.path.isfile(CONFIG_PATH):
        try:
            import shutil
            shutil.copy2(old, CONFIG_PATH)
        except OSError:
            pass


def _cleanup_old_data():
    """Після переїзду в AppData прибираємо старі дані ПОРУЧ із .exe, щоб не
    засмічували теку програми. Обережно: config видаляємо ЛИШЕ коли новий (у
    AppData) уже на місці, і шляхи справді різні (у dev вони збігаються — там
    нічого не чіпаємо, бо _migrate/_cleanup працюють лише в зібраній програмі).
    Стара тека profile — то лише кеш браузера; новий будується в AppData, тож
    стару прибираємо цілком (помилки ігноруємо, якщо щось зайняте)."""
    if not getattr(sys, "frozen", False):
        return
    import shutil
    old_cfg = os.path.join(BASE_DIR, "config.json")
    if (os.path.isfile(old_cfg) and os.path.isfile(CONFIG_PATH)
            and os.path.abspath(old_cfg) != os.path.abspath(CONFIG_PATH)):
        try:
            os.remove(old_cfg)
        except OSError:
            pass
    old_profile = os.path.join(BASE_DIR, "profile")
    new_profile = os.path.join(DATA_DIR, "profile")
    if (os.path.isdir(old_profile)
            and os.path.abspath(old_profile) != os.path.abspath(new_profile)):
        shutil.rmtree(old_profile, ignore_errors=True)


_migrate_old_config()
_cleanup_old_data()


def profile_dir() -> str:
    """Тека профілю браузера: кеш сторінок YouTube і його власні куки згоди.

    У даних програми (AppData), поруч із config.json — не приховано. Якщо туди
    чомусь не пишеться, відступаємо в тимчасову теку: інакше кожен запуск заново
    тягнув би кілька мегабайт скриптів YouTube.
    """
    here = os.path.join(DATA_DIR, "profile")
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
