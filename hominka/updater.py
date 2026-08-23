"""
Оновлення Hominka з update.svitix.com.

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
випуск, який знає обидва ключі, потім перехід на новий.
"""

import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass, field
from threading import Thread
from urllib.request import Request, urlopen

from PySide6.QtCore import QObject, Signal

from . import signing

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
IS_WINDOWS = sys.platform == "win32"

# Канали оновлень. Порядок = від найспокійнішого до найсвіжішого.
CHANNELS = [
    ("stable", "Стабільна", "Перевірені випуски. Рекомендовано."),
    ("beta", "Бета", "Свіжі можливості до того, як вони потраплять у стабільну."),
    ("dev", "Тестова", "Збірки одразу після змін. Можуть ламатися."),
]
DEFAULT_CHANNEL = "stable"

# Як показувати вид оновлення.
KIND_LABELS = {
    "major": "велике оновлення",
    "minor": "нові можливості",
    "patch": "виправлення",
    "hotfix": "термінове виправлення",
}

NET_TIMEOUT = 20          # с — на маніфест
DOWNLOAD_TIMEOUT = 600    # с — на архів (він великий)
USER_AGENT = "Hominka-Updater"


def channel_label(channel: str) -> str:
    for cid, label, _ in CHANNELS:
        if cid == channel:
            return label
    return channel


def kind_label(kind: str) -> str:
    return KIND_LABELS.get(kind, kind or "оновлення")


# --- версії ----------------------------------------------------------------

def parse_version(s: str) -> tuple:
    """«1.10.2» → (1, 10, 2). Нецифрові хвости («1.2.0-beta») відкидаємо:
    канал і так відомий, а порівнювати треба саме числа."""
    nums = re.findall(r"\d+", (s or "").split("+")[0])
    parts = [int(n) for n in nums[:3]]
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts)


def is_newer(candidate: str, current: str) -> bool:
    return parse_version(candidate) > parse_version(current)


# --- випуск ----------------------------------------------------------------

@dataclass
class Release:
    channel: str = ""
    version: str = ""
    kind: str = ""
    notes: str = ""
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


def _http_get(url: str, timeout: int) -> bytes:
    req = Request(url, headers={"User-Agent": USER_AGENT, "Cache-Control": "no-cache"})
    with urlopen(req, timeout=timeout) as resp:
        return resp.read()


# --- сам оновлювач ---------------------------------------------------------

class Updater(QObject):
    """Перевірка, завантаження і встановлення. Робота йде в окремих потоках,
    результати приходять сигналами — вікно чату не має підвисати."""

    # Release або None (оновлень немає); другий аргумент — текст помилки.
    checked = Signal(object, str)
    progress = Signal(int, int)   # завантажено, всього (байт)
    downloaded = Signal(str)      # шлях до перевіреного архіву
    failed = Signal(str)

    def __init__(self, current_version: str, app_dir: str, parent=None):
        super().__init__(parent)
        self.current_version = current_version
        self.app_dir = app_dir
        self._busy = False

    @property
    def busy(self) -> bool:
        return self._busy

    # --- перевірка ---
    def check(self, channel: str, installed_channel: str = ""):
        if self._busy:
            return
        self._busy = True
        Thread(target=self._check, args=(channel, installed_channel), daemon=True).start()

    def _check(self, channel: str, installed_channel: str):
        try:
            raw = _http_get(UPDATE_BASE + channel + ".json", NET_TIMEOUT)
            rel = parse_manifest(raw, channel)
            # Оновлення потрібне, якщо версія новіша АБО користувач щойно
            # переключив канал: перехід зі свіжої бети на стабільну — це теж
            # оновлення, хоч номер там і менший.
            switched = bool(installed_channel) and installed_channel != channel
            if is_newer(rel.version, self.current_version) or (
                switched and rel.version != self.current_version
            ):
                self.checked.emit(rel, "")
            else:
                self.checked.emit(None, "")
        except Exception as e:  # мережа, DNS, зіпсований маніфест
            self.checked.emit(None, str(e))
        finally:
            self._busy = False

    # --- завантаження ---
    def download(self, rel: Release):
        if self._busy:
            return
        self._busy = True
        Thread(target=self._download, args=(rel,), daemon=True).start()

    def _download(self, rel: Release):
        tmp = ""
        try:
            fd, tmp = tempfile.mkstemp(prefix="hominka-", suffix=".zip")
            os.close(fd)
            req = Request(rel.url, headers={"User-Agent": USER_AGENT})
            digest = hashlib.sha256()
            done = 0
            with urlopen(req, timeout=DOWNLOAD_TIMEOUT) as resp, open(tmp, "wb") as out:
                total = int(resp.headers.get("Content-Length") or rel.size or 0)
                while True:
                    chunk = resp.read(256 * 1024)
                    if not chunk:
                        break
                    out.write(chunk)
                    digest.update(chunk)
                    done += len(chunk)
                    self.progress.emit(done, total)
            got = digest.hexdigest()
            if rel.sha256 and got != rel.sha256:
                # Обірване завантаження або підміна — розпаковувати не можна.
                raise ValueError("контрольна сума не збіглася")
            if not zipfile.is_zipfile(tmp):
                raise ValueError("завантажений файл не є архівом")
            self.downloaded.emit(tmp)
        except Exception as e:
            if tmp and os.path.exists(tmp):
                try:
                    os.remove(tmp)
                except OSError:
                    pass
            self.failed.emit(str(e))
        finally:
            self._busy = False


def cleanup_downloads(keep: str = ""):
    """Прибирає завантажені архіви оновлень із тимчасової теки.

    Кожен архів — це 220 МБ. Якщо користувач завантажив оновлення й не поставив
    (закрив програму, передумав), файл лишиться лежати назавжди: тимчасову теку
    Windows сама не чистить. Тому підчищаємо і чужі минулі завантаження, і своє
    поточне — після встановлення воно вже ні до чого.

    keep — файл, який чіпати не можна (той, що зараз ставимо).
    """
    removed = 0
    pattern = os.path.join(tempfile.gettempdir(), "hominka-*.zip")
    for path in glob.glob(pattern):
        if keep and os.path.abspath(path) == os.path.abspath(keep):
            continue
        try:
            os.remove(path)
            removed += 1
        except OSError:
            pass          # файл ще тримає інший процес — приберемо наступного разу
    return removed


# --- встановлення ----------------------------------------------------------

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
