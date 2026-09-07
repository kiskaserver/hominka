"""Запуск інжектора: знайти нативні бінарі й покласти overlay.dll у гру.

Сам інжектор і бібліотека — native/ (C++). Тут лише Python-обгортка: знайти їх
поруч, вибрати розрядність під ціль, запустити й пояснити результат людськими
словами. Уся тверда відмова (античити) — у самому інжекторі (common/guard.h),
не тут: перевірку не можна лишати на боці, який легше обійти.
"""

import ctypes
import os
import subprocess
import sys
# wintypes існує лише на Windows: там, де його немає, сам імпорт кидає
# помилку, і модуль не завантажився б узагалі. Усе, що ним користується, і так
# під «if IS_WINDOWS».
if sys.platform == "win32":
    from ctypes import wintypes
else:
    wintypes = None

from .paths import BASE_DIR, resource_path

IS_WINDOWS = sys.platform == "win32"

# Токен запуску інжектора — дзеркало HOMINKA_INJECT_TOKEN з native/common/secret.h.
# Без нього інжектор відмовляє: щоб ним не користувалися окремо від програми.
INJECT_TOKEN = "HMK-INJ-7F3A9C21-64bd-4e0a-choose-your-game"

# Коди виходу інжектора (native/injector/injector.cpp).
EX_OK = 0
EX_ARGS = 1
EX_NO_PROC = 2
EX_BLOCKED = 3
EX_BITNESS = 4
EX_INJECT = 5


# За яким файлом упізнаємо теку з нативними частинами. Під Windows це
# інжектор, поза ним — сам рендер: ані injector.exe, ані overlay.dll на Linux
# не існує, і шукати їх там означало б не знайти теку взагалі.
_MARKER = "injector-x64.exe" if sys.platform == "win32" else "hominka-render-linux"


def native_dir() -> str:
    """Де лежать нативні частини.

    У зібраній програмі — поруч з виконуваним файлом (їх кладе туди release),
    у dev — у native/dist. Беремо перший каталог, де маркер реально лежить, а
    не просто той, що існує: інакше в dev ми б спинилися на порожньому native/.
    """
    cands = (os.path.join(BASE_DIR, "native"),
             resource_path("native"),
             os.path.join(BASE_DIR, "native", "dist"),
             os.path.join(os.path.dirname(BASE_DIR), "native", "dist"))
    for cand in cands:
        if cand and os.path.isfile(os.path.join(cand, _MARKER)):
            return cand
    return os.path.join(BASE_DIR, "native")


def available() -> bool:
    d = native_dir()
    return (os.path.isfile(os.path.join(d, "injector-x64.exe")) and
            os.path.isfile(os.path.join(d, "overlay-x64.dll")))


def _pid_of(hwnd: int) -> int:
    pid = wintypes.DWORD()
    ctypes.windll.user32.GetWindowThreadProcessId(wintypes.HWND(hwnd), ctypes.byref(pid))
    return int(pid.value)


# DLL графічних API, які вміє малювати наш оверлей. Якщо в процесі не завантажено
# ЖОДНОЇ — це не гра (нотатник, провідник, консоль…), і вкладатися туди нема сенсу.
GRAPHICS_DLLS = ("d3d9.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll",
                 "opengl32.dll", "vulkan-1.dll")


def _loaded_modules(pid: int):
    """Імена завантажених у процес DLL (нижнім регістром) або None, якщо
    перелічити не вдалося (гра від адміністратора тощо). None ≠ «порожньо»:
    у такому разі НЕ блокуємо — хай вирішує інжектор і сам оверлей."""
    if not IS_WINDOWS:
        return None
    k32 = ctypes.windll.kernel32
    psapi = ctypes.windll.psapi
    # Без argtypes ctypes бере HANDLE/HMODULE за c_int і переповнюється на 64-бітних
    # значеннях (адреси > 2 ГБ) — саме на цьому падав GetModuleBaseNameW.
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.EnumProcessModulesEx.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
    psapi.GetModuleBaseNameW.restype = wintypes.DWORD
    psapi.GetModuleBaseNameW.argtypes = [
        wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]

    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        return None
    try:
        arr = (wintypes.HMODULE * 1024)()
        needed = wintypes.DWORD()
        LIST_MODULES_ALL = 0x03   # і 32-, і 64-бітні модулі (важливо для WOW64-цілі)
        if not psapi.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr),
                                          ctypes.byref(needed), LIST_MODULES_ALL):
            return None
        count = min(len(arr), needed.value // ctypes.sizeof(wintypes.HMODULE))
        names = set()
        buf = ctypes.create_unicode_buffer(260)
        for i in range(count):
            if psapi.GetModuleBaseNameW(h, arr[i], buf, 260):
                names.add(buf.value.lower())
        return names
    finally:
        k32.CloseHandle(h)


def _has_graphics_api(pid: int) -> bool:
    """Чи схоже, що в процесі є гра з підтримуваним графічним API. Якщо модулі
    перелічити не вдалося — вважаємо, що так (краще спробувати, ніж дарма
    відмовити грі від адміністратора)."""
    mods = _loaded_modules(pid)
    if mods is None:
        return True
    return any(dll in mods for dll in GRAPHICS_DLLS)


def _target_is_64(pid: int) -> bool:
    """64-бітна ціль? Потрібне, щоб одразу взяти правильний інжектор і не
    ганяти його даремно на явно не тій розрядності."""
    if not IS_WINDOWS:
        return True
    k32 = ctypes.windll.kernel32
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return True   # не змогли дізнатись — хай вирішує сам інжектор
    try:
        wow = wintypes.BOOL()
        if k32.IsWow64Process(h, ctypes.byref(wow)):
            # WOW64 = 32-бітний процес на 64-бітній системі.
            return not wow.value
        return True
    finally:
        k32.CloseHandle(h)


class Result:
    def __init__(self, code: int, message: str, pid: int = 0):
        self.code = code
        self.ok = code == EX_OK
        self.message = message
        self.pid = pid          # PID гри — щоб малювати чат лише в ній


def inject(hwnd: int) -> Result:
    """Кладе overlay.dll у процес вікна hwnd. Повертає людяне пояснення."""
    if not IS_WINDOWS:
        return Result(EX_INJECT, "Інжектор лише для Windows.")
    if not available():
        return Result(EX_ARGS, "Нативні файли не знайдено — перевстановіть програму.")

    pid = _pid_of(hwnd)
    if not pid:
        return Result(EX_NO_PROC, "Не вдалося визначити гру у фокусі.")

    # Вкладаємося ЛИШЕ в ігри з підтримуваним графічним API (DirectX 9/11/12,
    # OpenGL, Vulkan). У звичайну програму без нього оверлею нема куди малювати —
    # і робити з неї «ціль» безглуздо, тож чесно відмовляємо ще до вкладення.
    if not _has_graphics_api(pid):
        return Result(EX_BLOCKED,
                      "У цьому вікні немає гри з підтримуваним графічним API "
                      "(DirectX 9/11/12, OpenGL чи Vulkan) — оверлей працює лише "
                      "в іграх, тож вкладати чат сюди нема куди.")

    d = native_dir()
    order = ["x64", "x86"] if _target_is_64(pid) else ["x86", "x64"]

    last = Result(EX_INJECT, "Не вдалося показати чат у грі.")
    for arch in order:
        exe = os.path.join(d, "injector-%s.exe" % arch)
        dll = os.path.join(d, "overlay-%s.dll" % arch)
        if not (os.path.isfile(exe) and os.path.isfile(dll)):
            continue
        try:
            # --token обовʼязковий: без нього інжектор не працює (захист від
            # запуску сторонніми). Шлях до DLL НЕ передаємо — інжектор бере лише
            # власну overlay й звіряє в ній маркер.
            proc = subprocess.run(
                [exe, "--pid", str(pid), "--token", INJECT_TOKEN],
                capture_output=True, text=True, timeout=20,
                creationflags=0x08000000)   # CREATE_NO_WINDOW
        except (OSError, subprocess.TimeoutExpired) as e:
            last = Result(EX_INJECT, "Інжектор не запустився: %s" % e)
            continue
        code = proc.returncode
        if code == EX_OK:
            return Result(EX_OK, "Чат у грі увімкнено.", pid)
        if code == EX_BLOCKED:
            return Result(EX_BLOCKED,
                          "Ця гра із захистом від сторонніх програм (античит) — "
                          "вкладати щось у неї не можна, це загрожує баном. "
                          "Для таких ігор лишається безрамковий режим.")
        if code == EX_BITNESS:
            # Не та розрядність — пробуємо інший інжектор.
            last = Result(EX_BITNESS, "Не та розрядність — пробую інший інжектор…")
            continue
        if code == EX_NO_PROC:
            last = Result(EX_NO_PROC, "Гра закрилася, поки ми до неї йшли.")
            continue
        if code == EX_INJECT:
            last = Result(EX_INJECT,
                          "Система не пустила в процес гри. Якщо гра запущена від "
                          "імені адміністратора, запустіть так само і Hominka.")
            continue
        last = Result(code, (proc.stderr or "Не вдалося показати чат у грі.").strip())
    return last
