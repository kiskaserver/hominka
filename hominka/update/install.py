"""Встановлення: розпакувати поруч і віддати підміну окремому процесу.

Запущену програму не можна перезаписати самою собою (у Windows файл узагалі
заблокований), тому копіює нове й перезапускає нас окремий процес — він і
чекає нашого виходу.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

from .download import cleanup_downloads

IS_WINDOWS = sys.platform == "win32"


def _parent_pid() -> int:
    """PID батьківського процесу (у збірці одним файлом — bootloader).

    Без сторонніх бібліотек: знімок процесів через Toolhelp і пошук себе.
    """
    if not IS_WINDOWS:
        return 0
    import ctypes
    from ctypes import wintypes

    class ENTRY(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", ctypes.c_char * 260)]

    kernel32 = ctypes.windll.kernel32
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)   # TH32CS_SNAPPROCESS
    if snapshot == -1:
        return 0
    try:
        entry = ENTRY()
        entry.dwSize = ctypes.sizeof(ENTRY)
        me = os.getpid()
        if not kernel32.Process32First(snapshot, ctypes.byref(entry)):
            return 0
        while True:
            if entry.th32ProcessID == me:
                return int(entry.th32ParentProcessID)
            if not kernel32.Process32Next(snapshot, ctypes.byref(entry)):
                return 0
    finally:
        kernel32.CloseHandle(snapshot)


def clean_env() -> dict:
    """Оточення для процесу-підмінника — без службових змінних PyInstaller.

    Bootloader кладе в оточення свої `_PYI_*` (у старих версіях `_MEIPASS2`),
    і дочірні процеси їх успадковують. Новий .exe, запущений з таким
    оточенням, вважає себе продовженням ЧУЖОГО запуску і зустрічає людину
    віконцем «_PYI_APPLICATION_HOME_DIR environment variable is not defined».
    Саме це й бачили ті, хто оновлювався з 1.8.0.
    """
    return {k: v for k, v in os.environ.items()
            if not k.startswith("_PYI_") and k not in ("_MEIPASS2", "_MEIPASS")}


def install(zip_path: str, app_dir: str) -> str:
    """Розпаковує архів поруч і запускає підмінник.

    Повертає команду, яку запустили (для журналу), або кидає виняток.

    Запущену програму не можна перезаписати саму собою (у Windows файл узагалі
    заблокований), тому підміну робить окремий процес: він чекає, поки ми
    закриємось, копіює нове поверх старого і запускає програму знову.
    Копіюємо, а не перейменовуємо теку, — щоб вціліли config.json і будь-що
    інше, що користувач поклав поруч.
    """
    if not IS_WINDOWS:
        return _install_posix(zip_path, app_dir)
    staging = os.path.join(os.path.dirname(app_dir.rstrip("\\/")), "Hominka_update")
    if os.path.exists(staging):
        shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging, exist_ok=True)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(staging)
    # Розпакували — архів більше не потрібен, як і все, що лишилося від
    # попередніх завантажень.
    try:
        os.remove(zip_path)
    except OSError:
        pass
    cleanup_downloads()

    # Архів може бути запакований як «Hominka/...» — тоді працюємо з підтекою.
    inner = os.path.join(staging, "Hominka")
    src = inner if os.path.isfile(os.path.join(inner, "Hominka.exe")) else staging
    if not os.path.isfile(os.path.join(src, "Hominka.exe")):
        raise ValueError("в архіві немає Hominka.exe")

    bat = os.path.join(tempfile.gettempdir(), "hominka-update.bat")
    with open(bat, "w", encoding="cp1251", errors="replace") as f:
        f.write(_UPDATE_BAT)
    # Чекати треба не лише на себе.
    #
    # Збірка одним файлом — це ДВА процеси: bootloader (він тримає .exe
    # відкритим) і ми, Python усередині нього. Підмінник раніше чекав лише на
    # наш pid, починав копіювати, поки батьківський ще живий, і копія
    # обривалася на замкненому файлі. Наслідок людина бачила як
    # «Failed to load Python DLL … python313.dll» — тобто .exe на диску
    # лишався битим.
    cmd = [os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "cmd.exe"),
           "/c", bat, str(os.getpid()), src, app_dir.rstrip("\\/"), staging,
           str(_parent_pid())]
    # CREATE_NO_WINDOW: підмінник переживає наш вихід і сам по собі (це окремий
    # процес), а вікна консолі посеред гри користувачу не потрібно. Повне
    # від'єднання (DETACHED_PROCESS) залишає його без консолі зовсім, і частина
    # системних утиліт у такому оточенні поводиться інакше.
    subprocess.Popen(cmd, creationflags=0x08000000 | 0x00000200, close_fds=True,
                     env=clean_env())
    return " ".join(cmd)


def _install_posix(zip_path: str, app_dir: str) -> str:
    """Те саме для Linux: чекаємо виходу, копіюємо, повертаємо право на запуск.

    Права з zip не приїжджають (формат їх не зберігає в тому вигляді, на який
    можна покластися), тому chmod робимо самі — інакше після оновлення файл
    просто не запуститься.
    """
    staging = os.path.join(os.path.dirname(app_dir.rstrip("/")), "Hominka_update")
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging, exist_ok=True)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(staging)
    try:
        os.remove(zip_path)
    except OSError:
        pass
    cleanup_downloads()

    inner = os.path.join(staging, "Hominka")
    src = inner if os.path.isfile(os.path.join(inner, "Hominka")) else staging
    if not os.path.isfile(os.path.join(src, "Hominka")):
        raise ValueError("в архіві немає Hominka")

    sh = os.path.join(tempfile.gettempdir(), "hominka-update.sh")
    with open(sh, "w", encoding="utf-8", newline=chr(10)) as f:
        f.write(_UPDATE_SH)
    os.chmod(sh, 0o755)
    cmd = ["/bin/sh", sh, str(os.getpid()), src, app_dir.rstrip("/"), staging]
    subprocess.Popen(cmd, start_new_session=True, close_fds=True, env=clean_env())
    return " ".join(cmd)


# $1 — pid програми, $2 — тека з новою версією, $3 — тека програми,
# $4 — тимчасова тека, яку треба прибрати.
_UPDATE_SH = r"""#!/bin/sh
LOG="${TMPDIR:-/tmp}/hominka-update.log"
echo "[$(date)] wait pid=$1 src=$2 dst=$3" >> "$LOG"
while kill -0 "$1" 2>/dev/null; do sleep 1; done
cp -a "$2/." "$3/" >> "$LOG" 2>&1
chmod +x "$3/Hominka" >> "$LOG" 2>&1
echo "[$(date)] copied, restarting" >> "$LOG"
("$3/Hominka" >/dev/null 2>&1 &)
rm -rf "$4"
rm -f "$0"
"""


# %1 — pid програми, %2 — тека з новою версією, %3 — тека програми,
# %4 — тимчасова тека, яку треба прибрати.
#
# Усі команди — повними шляхами з System32. Інакше все залежить від PATH: у
# системі з встановленим Git у PATH раніше трапляється його find.exe, який на
# ті самі аргументи відповідає інакше, і чекання завершення програми ламається
# мовчки — оновлення просто не встановлюється.
#
# ping замість timeout — у процесу без консолі timeout відмовляється працювати.
#
# Журнал у %TEMP%\hominka-update.log: підмінник працює вже після того, як вікно
# закрилося, і показати помилку йому нікуди.
_UPDATE_BAT = r"""@echo off
setlocal enabledelayedexpansion
set SYS=%SystemRoot%\System32
set LOG=%TEMP%\hominka-update.log
echo [%DATE% %TIME%] wait pid=%1 parent=%5 src=%2 dst=%3 >> "%LOG%"

REM Чекаємо і на Python-процес, і на bootloader: другий тримає .exe відкритим,
REM і копіювати поверх нього означає зіпсувати файл.
:wait
%SYS%	asklist.exe /NH /FI "PID eq %1" 2>nul | %SYS%ind.exe "%1" >nul
if not errorlevel 1 (
  %SYS%\ping.exe -n 2 127.0.0.1 >nul
  goto wait
)
if not "%~5"=="0" (
  :waitparent
  %SYS%	asklist.exe /NH /FI "PID eq %5" 2>nul | %SYS%ind.exe "%5" >nul
  if not errorlevel 1 (
    %SYS%\ping.exe -n 2 127.0.0.1 >nul
    goto waitparent
  )
)

REM Навіть після виходу процесу файл ще секунду-дві буває замкнений. Пробуємо
REM перейменувати його — це найдешевша перевірка «чи можна писати».
set TRIES=0
:trylock
set /a TRIES+=1
%SYS%\ping.exe -n 2 127.0.0.1 >nul
ren "%~3\Hominka.exe" "Hominka.exe.old" 2>nul
if errorlevel 1 (
  if !TRIES! LSS 15 goto trylock
  echo [%DATE% %TIME%] exe still locked, copying anyway >> "%LOG%"
) else (
  ren "%~3\Hominka.exe.old" "Hominka.exe" 2>nul
)

echo [%DATE% %TIME%] copying >> "%LOG%"
%SYS%obocopy.exe %2 %3 /E /IS /IT /R:5 /W:2 /NFL /NDL /NJH /NJS >> "%LOG%" 2>&1
echo [%DATE% %TIME%] robocopy exit=%ERRORLEVEL% >> "%LOG%"

REM Звіряємо розміри: обірвана копія — це саме те, через що людина бачила
REM «Failed to load Python DLL» замість програми.
for %%A in ("%~2\Hominka.exe") do set SRCSIZE=%%~zA
for %%A in ("%~3\Hominka.exe") do set DSTSIZE=%%~zA
echo [%DATE% %TIME%] size src=!SRCSIZE! dst=!DSTSIZE! >> "%LOG%"
if not "!SRCSIZE!"=="!DSTSIZE!" (
  echo [%DATE% %TIME%] size mismatch, retry once >> "%LOG%"
  %SYS%\ping.exe -n 4 127.0.0.1 >nul
  %SYS%obocopy.exe %2 %3 /E /IS /IT /R:5 /W:2 /NFL /NDL /NJH /NJS >> "%LOG%" 2>&1
  for %%A in ("%~3\Hominka.exe") do set DSTSIZE=%%~zA
  echo [%DATE% %TIME%] size after retry dst=!DSTSIZE! >> "%LOG%"
)

REM Перехід зі збірки текою на збірку одним файлом: _internal у новій версії
REM немає, а стара його лишила — 340 МБ, які вже нікому не потрібні.
if not exist "%~2\_internal" if exist "%~3\_internal" rmdir /s /q "%~3\_internal"
start "" "%~3\Hominka.exe"
rmdir /s /q %4
(goto) 2>nul & del "%~f0"
"""
