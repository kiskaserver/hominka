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

Чого тут свідомо немає: підпису випусків. Цілісність тримається на HTTPS до
update.svitix.com і на sha256 у маніфесті звідти ж. Хто зможе писати в цей
домен — зможе підсунути код. Місце для підпису в маніфесті передбачене
(поле "signature"), вмикати його є сенс, якщо оновлення роздаватимуться
далі за коло своїх.
"""

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

# Базова адреса оновлень. Змінюється разом із доменом, тому окремою константою.
UPDATE_BASE = "https://update.svitix.com/hominka/"

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


def parse_manifest(raw: bytes, channel: str) -> Release:
    data = json.loads(raw.decode("utf-8"))
    f = data.get("file") or {}
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
    # Завантажувати будемо тільки з нашого домену: маніфест теж приїхав із
    # мережі, і посилання «кудись іще» — привід зупинитися, а не качати.
    if not rel.url.startswith(UPDATE_BASE):
        raise ValueError("посилання на файл поза %s" % UPDATE_BASE)
    return rel


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


# --- встановлення ----------------------------------------------------------

def install(zip_path: str, app_dir: str) -> str:
    """Розпаковує архів поруч і запускає підмінник.

    Повертає команду, яку запустили (для журналу), або кидає виняток.

    Windows тримає запущений .exe заблокованим, тому підміняє теку окремий
    процес: він чекає, поки ми закриємось, копіює нове поверх старого і
    запускає програму знову. Копіюємо, а не перейменовуємо теку, — щоб
    вціліли config.json і будь-що інше, що користувач поклав поруч.
    """
    staging = os.path.join(os.path.dirname(app_dir.rstrip("\\/")), "Hominka_update")
    if os.path.exists(staging):
        shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging, exist_ok=True)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(staging)
    try:
        os.remove(zip_path)
    except OSError:
        pass

    # Архів може бути запакований як «Hominka/...» — тоді працюємо з підтекою.
    inner = os.path.join(staging, "Hominka")
    src = inner if os.path.isfile(os.path.join(inner, "Hominka.exe")) else staging
    if not os.path.isfile(os.path.join(src, "Hominka.exe")):
        raise ValueError("в архіві немає Hominka.exe")

    bat = os.path.join(tempfile.gettempdir(), "hominka-update.bat")
    with open(bat, "w", encoding="cp1251", errors="replace") as f:
        f.write(_UPDATE_BAT)
    cmd = ["cmd", "/c", bat, str(os.getpid()), src, app_dir.rstrip("\\/"), staging]
    # DETACHED_PROCESS — щоб підмінник пережив наш вихід.
    subprocess.Popen(cmd, creationflags=0x00000008 | 0x00000200, close_fds=True)
    return " ".join(cmd)


# %1 — pid програми, %2 — тека з новою версією, %3 — тека програми,
# %4 — тимчасова тека, яку треба прибрати.
_UPDATE_BAT = r"""@echo off
setlocal
:wait
tasklist /FI "PID eq %1" 2>nul | find "%1" >nul
if not errorlevel 1 (
  ping -n 2 127.0.0.1 >nul
  goto wait
)
robocopy %2 %3 /E /IS /IT /R:2 /W:1 /NFL /NDL /NJH /NJS >nul
start "" "%~3\Hominka.exe"
rmdir /s /q %4
(goto) 2>nul & del "%~f0"
"""
