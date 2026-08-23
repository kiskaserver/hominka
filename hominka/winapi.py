"""Windows-частина: невидимість для OBS, клік-крізь, гаряча клавіша.

Усе, що тут є, існує заради одного трюку — вікна, якого не бачить захоплення
екрана (SetWindowDisplayAffinity + WDA_EXCLUDEFROMCAPTURE). Поза Windows такого
немає ні в X11, ні у Wayland, тому кожна функція вміє нічого не робити.
"""

import ctypes
import os
import sys
from ctypes import wintypes

from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtWidgets import QApplication, QWidget

# === Платформа ==============================================================
#
# Вікно, невидиме для OBS, — це Windows-трюк (SetWindowDisplayAffinity). У
# Linux такого немає ні в X11, ні у Wayland: жодне API не дозволяє сказати
# «мене не знімати». Але воно там і не потрібне так гостро — на Linux стрімер
# знімає ГРУ, а не екран: OBS має «Window Capture (Xcomposite)» під X11,
# «Window Capture (PipeWire)» під Wayland і obs-vkcapture для ігор. Усі троє
# знімають одне конкретне вікно, а наше — інше, тож у кадр воно не потрапляє.
# Ловить оверлей лише захоплення всього екрана — про це й пишемо людині.
IS_WINDOWS = sys.platform == "win32"


# === WinAPI константи =======================================================
WDA_EXCLUDEFROMCAPTURE = 0x00000011


GWL_EXSTYLE = -20


WS_EX_TRANSPARENT = 0x00000020


WS_EX_LAYERED = 0x00080000


WM_HOTKEY = 0x0312


MOD_CONTROL = 0x0002


MOD_ALT = 0x0001


MOD_NOREPEAT = 0x4000


VK_SPACE = 0x20


HOTKEY_ID = 1

user32 = ctypes.windll.user32 if IS_WINDOWS else None


def _hwnd(win) -> int:
    return int(win.winId())


# Атрибут «прихований» у Windows.
FILE_ATTRIBUTE_HIDDEN = 0x2


def hide_internal_folder():
    """Ховає службову теку профілю поруч із програмою.

    Тека _internal тут більше не з'являється: збірка одним файлом тримає все
    всередині .exe (див. Hominka_one.spec). Але після оновлення зі старої
    версії вона може лишитися на диску — тоді ховаємо і її, щоб не плуталася
    під ногами.

    Робиться щоразу при старті: після оновлення теки копіюють наново, і атрибут
    з них злітає.
    """
    if not getattr(sys, "frozen", False) or not IS_WINDOWS:
        return
    here = os.path.dirname(sys.executable)
    # profile — кеш браузера, службовий. config.json НЕ ховаємо: це
    # налаштування користувача, і шукати їх у прихованому — знущання.
    for name in ("_internal", "profile"):
        folder = os.path.join(here, name)
        if not os.path.isdir(folder):
            continue
        try:
            ctypes.windll.kernel32.SetFileAttributesW(folder, FILE_ATTRIBUTE_HIDDEN)
        except Exception:
            pass      # не вийшло приховати — не привід не запускатися


def exclude_from_capture(win) -> bool:
    if not IS_WINDOWS:
        return False
    try:
        return bool(user32.SetWindowDisplayAffinity(_hwnd(win), WDA_EXCLUDEFROMCAPTURE))
    except Exception:
        return False


def _is_excluded(hwnd: int) -> bool:
    """Чи вже приховане вікно від захоплення (щоб не смикати WinAPI даремно)."""
    if not IS_WINDOWS:
        return True
    value = wintypes.DWORD()
    try:
        if not user32.GetWindowDisplayAffinity(hwnd, ctypes.byref(value)):
            return False
    except Exception:
        return False
    return value.value == WDA_EXCLUDEFROMCAPTURE


class CaptureGuard(QObject):
    """Ховає від захоплення кожне вікно ще до того, як його намалюють.

    Обхід за таймером теж є (див. hide_new_windows_from_capture), але він
    запізнюється: між появою списку й наступним тактом таймера вікно вже
    встигає потрапити в кадр. Подія Show приходить раніше за перше малювання,
    тому ловимо саме її.
    """

    def eventFilter(self, obj, event):
        if event.type() == QEvent.Type.Show and isinstance(obj, QWidget) and obj.isWindow():
            exclude_from_capture(obj)
        return False


def hide_new_windows_from_capture():
    """Ховає від захоплення кожне вікно програми, яке цього ще не має.

    Головне вікно й налаштування ховають себе самі при показі, але цього мало:
    випадні списки, підказки й будь-які інші спливаючі елементи Qt — то ОКРЕМІ
    вікна, і створюються вони лише в мить появи. Саме такий список і потрапив
    на трансляцію: сама панель була прихована, а список каналів оновлень — ні.

    Дешевше пройтися по верхньорівневих вікнах, ніж вгадувати, яке з них Qt
    створить наступним.
    """
    if not IS_WINDOWS:
        return
    app = QApplication.instance()
    if app is None:
        return
    for w in app.topLevelWidgets():
        if not w.isVisible():
            continue
        try:
            hwnd = int(w.winId())
        except Exception:
            continue
        if hwnd and not _is_excluded(hwnd):
            user32.SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)


def set_click_through(win, enabled: bool):
    """Миша йде крізь вікно в гру.

    У Windows це прапорець стилю, який можна змінити на льоту. У Linux того ж
    домагаємось прапорцем вікна Qt — але він діє лише при показі, тому вікно
    доводиться перепоказати; геометрію при цьому зберігаємо самі, інакше
    менеджер вікон поставить його, куди йому зручно.
    """
    if IS_WINDOWS:
        hwnd = _hwnd(win)
        ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        if enabled:
            ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED
        else:
            ex &= ~WS_EX_TRANSPARENT
        user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex)
        return
    geo = win.geometry()
    win.setWindowFlag(Qt.WindowTransparentForInput, enabled)
    win.show()
    win.setGeometry(geo)


# === Пошук власної трансляції ===============================================
#
# Програма — для стрімера, який читає СВІЙ чат, тож вставляти посилання перед
# кожним ефіром безглуздо: досить один раз назвати свій канал (⚙ → «Мій
# канал»), і трансляція знайдеться сама.
#
# Канал → трансляція: сторінка youtube.com/channel/<id>/live. Коли ефір іде,
# YouTube сам переадресовує на сторінку перегляду, і в ytInitialData лежить
# currentVideoEndpoint з id відео; ефіру немає — це просто сторінка каналу.
#
# Входу в акаунт тут немає й не треба: обидві сторінки відкриті всім. Це
# принципово — Google не пускає у вбудований браузер, і будувати на вході щось
# робоче не вийшло (див. історію: вікно passkey і «request is malformed»).
#
# Ходимо звичайними сторінками — ніяких ключів і ніяких квот.
