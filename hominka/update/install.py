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
    cmd = [os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "cmd.exe"),
           "/c", bat, str(os.getpid()), src, app_dir.rstrip("\\/"), staging]
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
setlocal
set SYS=%SystemRoot%\System32
set LOG=%TEMP%\hominka-update.log
echo [%DATE% %TIME%] wait pid=%1 src=%2 dst=%3 >> "%LOG%"
:wait
%SYS%\tasklist.exe /NH /FI "PID eq %1" 2>nul | %SYS%\find.exe "%1" >nul
if not errorlevel 1 (
  %SYS%\ping.exe -n 2 127.0.0.1 >nul
  goto wait
)
echo [%DATE% %TIME%] copying >> "%LOG%"
%SYS%\robocopy.exe %2 %3 /E /IS /IT /R:2 /W:1 /NFL /NDL /NJH /NJS >> "%LOG%" 2>&1
echo [%DATE% %TIME%] robocopy exit=%ERRORLEVEL% >> "%LOG%"
REM Перехід зі збірки текою на збірку одним файлом: _internal у новій версії
REM немає, а стара його лишила — 340 МБ, які вже нікому не потрібні.
if not exist "%~2\_internal" if exist "%~3\_internal" rmdir /s /q "%~3\_internal"
start "" "%~3\Hominka.exe"
rmdir /s /q %4
(goto) 2>nul & del "%~f0"
"""
