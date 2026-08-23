"""Профіль вбудованого браузера: свій, постійний, поруч із програмою."""

import os

from PySide6.QtWebEngineCore import QWebEngineProfile

from .paths import profile_dir

# UA звичайного Chrome. За замовчуванням QtWebEngine пише в UA сам себе, і на
# такий рядок Google реагує окремо — аж до відмови у вході.
CHROME_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
             "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36")


PROFILE_DIR = profile_dir()

# UA звичайного Chrome. За замовчуванням QtWebEngine пише в UA сам себе, і на
# такий рядок Google реагує окремо — аж до відмови у вході.
CHROME_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
             "(KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36")


def build_profile(parent) -> QWebEngineProfile:
    """Постійний профіль браузера.

    Профіль за замовчуванням у Qt — «інкогніто»: нічого не зберігається, і
    кожен запуск качає сторінки YouTube з нуля. Іменований профіль лишає кеш і
    згоду на куки при собі. Входу в акаунт тут немає: Google не пускає у
    вбудований браузер, і програма навіть не пробує.
    """
    prof = QWebEngineProfile("hominka", parent)
    prof.setPersistentStoragePath(os.path.join(PROFILE_DIR, "storage"))
    prof.setCachePath(os.path.join(PROFILE_DIR, "cache"))
    prof.setPersistentCookiesPolicy(QWebEngineProfile.PersistentCookiesPolicy.ForcePersistentCookies)
    prof.setHttpUserAgent(CHROME_UA)
    return prof
