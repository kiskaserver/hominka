"""Чат поверх гри у справжньому повноекранному режимі — що тут можна, а що ні.

Коротко про суть проблеми. Коли гра просить **exclusive fullscreen**, вона
забирає вивід відеокарти собі, і композитор Windows (DWM) відходить убік —
малювати поверх стає нікому. Прапорець «поверх усіх вікон» тут ні до чого: він
про менеджер вікон, якого в цю мить немає.

Рятує те, що з 2017 року Windows підміняє це режимом **Fullscreen
Optimizations**: гра лишається композованою, виглядає так само, а оверлеї
поверх неї працюють. Тому сучасні ігри проблеми не мають. Лишаються дві
ситуації: старі ігри (DX9/DDraw), яким оптимізації не діють, і випадок, коли
людина сама поставила в властивостях .exe «Вимкнути оптимізації на весь екран».

Що вміє цей модуль:
  * тримати наше вікно зверху — але акуратно, лише коли змінилося вікно
    переднього плану (постійне смикання SetWindowPos і є причиною мерехтіння
    у старих іграх);
  * розпізнати, у якому режимі зараз гра, і сказати про це людині;
  * на прохання перевести вікно гри в безрамковий режим — те саме, що робить
    Borderless Gaming: зняти рамку і розтягнути на монітор. Тоді вікном керує
    DWM, і чат поверх нього видно.

Чого тут свідомо немає: інʼєкції в процес гри. Малювати всередині чужого кадру
означає підвантажити свою DLL у гру і перехопити Present — так роблять OBS,
Discord і Steam. Це окрема тема, окремою мовою і з окремими наслідками
(античити, антивіруси), тож у цьому файлі її немає.
"""

import ctypes
import os
import sys
# wintypes існує лише на Windows: там, де його немає, сам імпорт кидає помилку,
# і модуль не завантажився б узагалі. Усе, що ним користується, і так під
# «if IS_WINDOWS».
if sys.platform == "win32":
    from ctypes import wintypes
else:
    wintypes = None

from .winapi import IS_WINDOWS, user32

# --- WinAPI, якого немає у winapi.py (там лише про приховування від захоплення)
HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
SWP_FRAMECHANGED = 0x0020
SWP_SHOWWINDOW = 0x0040

GWL_STYLE = -16
GWL_EXSTYLE = -20

WS_CAPTION = 0x00C00000
WS_THICKFRAME = 0x00040000
WS_MINIMIZEBOX = 0x00020000
WS_MAXIMIZEBOX = 0x00010000
WS_SYSMENU = 0x00080000
WS_BORDER = 0x00800000
WS_DLGFRAME = 0x00400000
WS_POPUP = 0x80000000

WS_EX_DLGMODALFRAME = 0x00000001
WS_EX_CLIENTEDGE = 0x00000200
WS_EX_STATICEDGE = 0x00020000
WS_EX_WINDOWEDGE = 0x00000100

MONITOR_DEFAULTTONEAREST = 2
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

# Відповіді SHQueryUserNotificationState (shellapi.h). Нас цікавлять дві:
# D3D_FULL_SCREEN — саме той випадок, коли поверх нічого не намалюєш;
# BUSY — повноекранна програма, але композована (так відповідає система, коли
# працюють Fullscreen Optimizations), тобто оверлей буде видно.
QUNS_BUSY = 2
QUNS_RUNNING_D3D_FULL_SCREEN = 3
QUNS_PRESENTATION_MODE = 4


# Тіло класу виконується ПРИ ІМПОРТІ, тож поза Windows його не має бути
# взагалі: там wintypes немає, і модуль не завантажився б. Решта тутешнього —
# функції, а їхні тіла виконуються лише при виклику, і кличуть їх тільки під
# Windows (available() каже «ні»).
if IS_WINDOWS:
    class _MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD),
                    ("rcMonitor", wintypes.RECT),
                    ("rcWork", wintypes.RECT),
                    ("dwFlags", wintypes.DWORD)]


# Що ми змінили в чужих вікнах: hwnd → (style, exstyle, rect).
#
# Тримаємо в памʼяті, а не в config.json, навмисно: після перезапуску нашої
# програми гри вже немає, а «повернути як було» комусь іншому — гірше, ніж не
# повертати.
_changed = {}


def available() -> bool:
    return bool(IS_WINDOWS and user32)


def foreground() -> dict:
    """Хто зараз попереду: вікно, його назва, файл програми і прямокутник."""
    if not available():
        return {}
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return {}
    length = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buf, length + 1)
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return {"hwnd": hwnd, "title": buf.value, "exe": _exe_of(hwnd), "rect": rect}


def _full_exe_path(hwnd: int) -> str:
    """Повний шлях до .exe вікна (порожній рядок, якщо не вдалося)."""
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if not pid.value:
        return ""
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
    if not handle:
        return ""
    try:
        size = wintypes.DWORD(512)
        buf = ctypes.create_unicode_buffer(size.value)
        if kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return buf.value
    finally:
        kernel32.CloseHandle(handle)
    return ""


def _exe_of(hwnd: int) -> str:
    """Імʼя .exe вікна. Потрібне, щоб не чіпати ні себе, ні робочий стіл."""
    full = _full_exe_path(hwnd)
    return os.path.basename(full) if full else ""


def game_exe_path(hwnd: int) -> str:
    """Повний шлях до .exe гри у цьому вікні — для реєстру AppCompatFlags."""
    return _full_exe_path(int(hwnd)) if available() and hwnd else ""


# --- Fullscreen Optimizations (FSO) для конкретної гри ----------------------
# Безрамковий повноекранний під FSO Windows переводить у Independent Flip: кадр
# гри сканується повз композитор, і наш оверлей (та й будь-яке звичайне вікно
# зверху) зникає в 3D. Найнадійніше — вимкнути FSO саме для цієї гри: те саме, що
# галочка «Вимкнути оптимізацію на весь екран» у властивостях .exe. Технічно це
# рядок-прапорець DISABLEDXMAXIMIZEDWINDOWEDMODE у гілці сумісності HKCU. Діє з
# наступного запуску гри й нічого в саму гру не вкладає (безпечно для античитів).
_LAYERS_KEY = r"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
_FSO_FLAG = "DISABLEDXMAXIMIZEDWINDOWEDMODE"


def _layers_tokens(exe_path: str):
    """Поточні прапорці сумісності для .exe як список токенів (напр. ['~',
    'HIGHDPIAWARE']). Порожній список, якщо запису нема."""
    import winreg
    try:
        key = winreg.OpenKeyEx(winreg.HKEY_CURRENT_USER, _LAYERS_KEY, 0, winreg.KEY_READ)
    except OSError:
        return []
    try:
        val, _ = winreg.QueryValueEx(key, exe_path)
        return str(val).split()
    except OSError:
        return []
    finally:
        winreg.CloseKey(key)


def fullscreen_opt_disabled(exe_path: str) -> bool:
    """Чи вимкнено FSO для цієї гри зараз."""
    if not IS_WINDOWS or not exe_path:
        return False
    return _FSO_FLAG in _layers_tokens(exe_path)


def set_fullscreen_opt_disabled(exe_path: str, disabled: bool) -> bool:
    """Вмикає/вимикає прапорець «без FSO» для гри, зберігаючи інші прапорці
    сумісності. Повертає True, якщо вдалося записати."""
    if not IS_WINDOWS or not exe_path:
        return False
    import winreg
    tokens = _layers_tokens(exe_path)
    # «~» — обов'язковий маркер шару на початку; без нього прапорці ігноруються.
    tokens = [t for t in tokens if t not in ("~", _FSO_FLAG)]
    if disabled:
        tokens = ["~", _FSO_FLAG] + tokens
    elif tokens:
        tokens = ["~"] + tokens          # лишилися інші прапорці — тримаємо маркер
    try:
        key = winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, _LAYERS_KEY, 0, winreg.KEY_SET_VALUE)
    except OSError:
        return False
    try:
        if tokens:
            winreg.SetValueEx(key, exe_path, 0, winreg.REG_SZ, " ".join(tokens))
        else:
            try:
                winreg.DeleteValue(key, exe_path)   # нічого не лишилось — прибираємо запис
            except OSError:
                pass
        return True
    except OSError:
        return False
    finally:
        winreg.CloseKey(key)


WS_EX_TOOLWINDOW = 0x00000080
GW_OWNER = 4

_OURS = ("hominka.exe", "python.exe", "pythonw.exe")
_SKIP = ("explorer.exe", "applicationframehost.exe", "textinputhost.exe",
         "systemsettings.exe", "searchhost.exe", "shellexperiencehost.exe",
         "startmenuexperiencehost.exe", "")


def list_windows() -> list:
    """Список видимих вікон-кандидатів на гру: [(hwnd, pid, exe, title)].

    Замість «вгадай, що зараз попереду» — явний вибір зі списку. Беремо
    top-level вікна, у яких є заголовок, які видимі й не службові: саме такими
    бувають вікна ігор. Себе, робочий стіл і системну обслугу відкидаємо.
    """
    if not IS_WINDOWS:
        return []
    out = []
    seen = set()

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        # Дочірні/власні спливні вікна не показуємо — потрібне головне вікно гри.
        if user32.GetWindow(hwnd, GW_OWNER):
            return True
        ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        if ex & WS_EX_TOOLWINDOW:
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length <= 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        title = buf.value.strip()
        if not title:
            return True
        exe = _exe_of(hwnd)
        low = exe.lower()
        if low in _OURS or low in _SKIP:
            return True
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        key = (pid.value, low)
        if key in seen:              # одна гра — один рядок, навіть якщо вікон кілька
            return True
        seen.add(key)
        out.append((int(hwnd), int(pid.value), exe, title))
        return True

    user32.EnumWindows(cb, 0)
    out.sort(key=lambda w: w[3].lower())
    return out


def monitor_rect(hwnd: int):
    """Прямокутник монітора, на якому лежить вікно."""
    monitor = user32.MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
    info = _MONITORINFO()
    info.cbSize = ctypes.sizeof(_MONITORINFO)
    user32.GetMonitorInfoW(monitor, ctypes.byref(info))
    return info.rcMonitor


def notification_state() -> int:
    """SHQueryUserNotificationState — питання «чи зараз можна показувати вікна».

    Система відповідає на нього чесніше, ніж будь-яка перевірка стилів: саме
    вона знає, чи віддала екран у виключне користування Direct3D.
    """
    try:
        state = ctypes.c_int(0)
        if ctypes.windll.shell32.SHQueryUserNotificationState(ctypes.byref(state)) == 0:
            return state.value
    except Exception:
        pass
    return 0


def state() -> dict:
    """У якому режимі зараз гра — і що це означає для чату.

    Повертає: kind ∈ {none, desktop, windowed, borderless, exclusive},
    title, exe, і can_overlay — чи побачить людина чат поверх цього вікна.
    """
    info = foreground()
    if not info:
        return {"kind": "none", "title": "", "exe": "", "can_overlay": True}

    ours = info["exe"].lower() in ("hominka.exe", "python.exe", "pythonw.exe")
    shell = info["exe"].lower() in ("explorer.exe", "") and info["title"] in ("", "Program Manager")
    if ours or shell:
        return {"kind": "desktop", "title": info["title"], "exe": info["exe"],
                "can_overlay": True, "hwnd": info["hwnd"]}

    if notification_state() == QUNS_RUNNING_D3D_FULL_SCREEN:
        return {"kind": "exclusive", "title": info["title"], "exe": info["exe"],
                "can_overlay": False, "hwnd": info["hwnd"]}

    mon = monitor_rect(info["hwnd"])
    r = info["rect"]
    covers = (r.left <= mon.left and r.top <= mon.top
              and r.right >= mon.right and r.bottom >= mon.bottom)
    style = user32.GetWindowLongW(info["hwnd"], GWL_STYLE)
    framed = bool(style & (WS_CAPTION | WS_THICKFRAME))
    kind = "borderless" if covers and not framed else "windowed"
    return {"kind": kind, "title": info["title"], "exe": info["exe"],
            "can_overlay": True, "hwnd": info["hwnd"]}


def raise_topmost(win):
    """Пере-піднімає вікно на самий верх без активації/руху/розміру.

    Саме «поставити HWND_TOPMOST», а не перемикати TOPMOST↔NOTOPMOST: перемикання
    якраз і мерехтить, а повторне встановлення того самого topmost — дешева
    операція без видимого ефекту. Над повноекранною грою її треба робити часто:
    гра сидить у вищому z-band (FSO), і одноразового підняття мало."""
    if not available():
        return
    try:
        user32.SetWindowPos(int(win.winId()), HWND_TOPMOST, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
    except Exception:
        pass


def fullscreen_game():
    """Якщо попереду ПОВНОЕКРАННА гра — повертає (hwnd, монітор), інакше None.

    І «безрамковий» (FSO/незалежний flip), і «повноекранний» (SHQuery каже
    D3D-fullscreen) — усе це кейси, де гра сканується поверх композиції й накриває
    оверлей. Для звичайного вікна/столу цього не робимо. Класифікація — у state();
    тут просто зводимо два повноекранні різновиди в один сигнал."""
    st = state()
    if st.get("kind") in ("borderless", "exclusive"):
        hwnd = st.get("hwnd")
        if hwnd:
            return hwnd, monitor_rect(hwnd)
    return None


class TopMostKeeper:
    """Тримає наше вікно зверху, реагуючи на зміну вікна переднього плану.

    Це м'який, загальний випадок (перемикання між звичайними вікнами й столом);
    смикати щотакту тут ні до чого. Агресивне утримання над ПОВНОЕКРАННОЮ грою
    (де гра постійно перебиває z-band) робить окремий швидкий таймер у overlay.py
    разом із форсуванням композиції (compositor.py)."""

    def __init__(self, win):
        self.win = win
        self.enabled = True
        self._last = 0

    def tick(self):
        if not (self.enabled and available() and self.win.isVisible()):
            return
        current = user32.GetForegroundWindow()
        if current == self._last:
            return
        self._last = current
        raise_topmost(self.win)


def make_borderless(hwnd: int) -> bool:
    """Знімає з чужого вікна рамку і розтягує його на монітор.

    Те саме, що робить Borderless Gaming, і без жодної інʼєкції: стиль вікна —
    звичайна властивість, яку можна змінити ззовні. Гра після цього лишається
    «на весь екран» на вигляд, але керує нею DWM, а отже чат поверх неї видно.
    """
    if not available() or not hwnd or hwnd in _changed:
        return False
    style = user32.GetWindowLongW(hwnd, GWL_STYLE)
    exstyle = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    _changed[hwnd] = (style, exstyle, (rect.left, rect.top, rect.right, rect.bottom))

    new_style = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX
                           | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME))
    new_style |= WS_POPUP
    new_ex = exstyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE
                         | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE)
    user32.SetWindowLongW(hwnd, GWL_STYLE, new_style)
    user32.SetWindowLongW(hwnd, GWL_EXSTYLE, new_ex)

    mon = monitor_rect(hwnd)
    user32.SetWindowPos(hwnd, 0, mon.left, mon.top,
                        mon.right - mon.left, mon.bottom - mon.top,
                        SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW)
    return True


def restore(hwnd: int) -> bool:
    """Повертає вікну те, що в нього було до нашого втручання."""
    saved = _changed.pop(hwnd, None)
    if not saved or not available():
        return False
    style, exstyle, (left, top, right, bottom) = saved
    user32.SetWindowLongW(hwnd, GWL_STYLE, style)
    user32.SetWindowLongW(hwnd, GWL_EXSTYLE, exstyle)
    user32.SetWindowPos(hwnd, 0, left, top, right - left, bottom - top,
                        SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW)
    return True


def changed_window() -> int:
    """Вікно, яке ми зробили безрамковим (нуль, якщо таких немає)."""
    for hwnd in _changed:
        return hwnd
    return 0
