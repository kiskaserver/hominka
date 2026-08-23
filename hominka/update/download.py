"""Перевірка наявності оновлення і завантаження архіву.

Робота йде в окремих потоках, результати приходять сигналами: вікно чату не
має підвисати через мережу.
"""

import glob
import hashlib
import os
import tempfile
import zipfile
from threading import Thread
from urllib.request import Request, urlopen

from PySide6.QtCore import QObject, Signal

from .channels import is_newer
from .manifest import UPDATE_BASE, Release, parse_manifest

NET_TIMEOUT = 20          # с — на маніфест


DOWNLOAD_TIMEOUT = 600    # с — на архів (він великий)


USER_AGENT = "Hominka-Updater"


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
