"""Дублювання чату в OSD RivaTuner Statistics Server.

Навіщо. У справжньому виключному повноекранному режимі (старі ігри на DX9,
або коли людина сама вимкнула оптимізації) поверх гри не малює ніхто, крім
самої гри, — див. fullscreen.py. Єдиний спосіб щось там показати, не
підвантажуючи свою DLL у чужий процес, — попросити того, хто вже туди
підвантажився. RTSS саме такий: він хукає DX9/11/12, OpenGL і Vulkan заради
свого лічильника кадрів і дозволяє стороннім програмам писати власний текст у
свій OSD.

Як це влаштовано. RTSS тримає спільну памʼять `RTSSSharedMemoryV2` з масивом
слотів OSD. Кожен слот — це текст і імʼя власника. Ми знаходимо (або займаємо)
слот з іменем «Hominka», пишемо туди останні рядки чату і збільшуємо лічильник
кадрів — RTSS бачить зміну і перемальовує OSD у грі.

Чого чекати. Це ТЕКСТ: без аватарок, емоутів і кольорів ніків. Для старої гри,
де інакше чату немає взагалі, це краще, ніж нічого; для всього іншого є звичайне
вікно.

Структура спільної памʼяті взята з SDK RTSS (RTSSSharedMemory.h), заголовок:
signature, version, appEntrySize, appArrOffset, appArrSize, osdEntrySize,
osdArrOffset, osdArrSize, osdFrame — далі поля, які нам не потрібні.
"""

import ctypes
import os
import struct
import subprocess

MAP_NAME = "RTSSSharedMemoryV2"
OWNER = "Hominka"
SIGNATURE = 0x52545353          # 'RTSS' як багатосимвольна константа C
FILE_MAP_ALL_ACCESS = 0x000F001F

# Зсуви всередині запису слоту (див. RTSS_SHARED_MEMORY_OSD_ENTRY).
OSD_TEXT = 0                    # char szOSD[256]
OSD_OWNER = 256                 # char szOSDOwner[256]
OSD_TEXT_EX = 512               # char szOSDEx[4096]

kernel32 = ctypes.windll.kernel32 if hasattr(ctypes, "windll") else None


# Де RTSS зазвичай лежить і звідки його беруть.
RTSS_PATHS = (
    r"C:\Program Files (x86)\RivaTuner Statistics Server\RTSS.exe",
    r"C:\Program Files\RivaTuner Statistics Server\RTSS.exe",
)
RTSS_SITE = "https://www.guru3d.com/download/rtss-rivatuner-statistics-server-download/"


def installed_path() -> str:
    """Шлях до RTSS.exe, якщо він встановлений (інакше порожньо).

    Потрібне, щоб відрізнити «не встановлено» від «встановлено, але не
    запущено»: у першому випадку людині треба посилання, у другому — кнопка.
    """
    for path in RTSS_PATHS:
        if os.path.isfile(path):
            return path
    return ""


def launch() -> bool:
    """Запускає встановлений RTSS (він осідає в треї)."""
    path = installed_path()
    if not path:
        return False
    try:
        subprocess.Popen([path], cwd=os.path.dirname(path))
        return True
    except OSError:
        return False


class Bridge:
    """Місток до OSD RTSS: відкрити, писати рядки, прибрати за собою.

    Живе весь час роботи програми, але тримає ресурс лише поки ввімкнений:
    вимкнули галочку — слот звільняється, і RTSS про нас забуває.
    """

    def __init__(self):
        self.handle = None
        self.view = None
        self.slot = 0
        self.error = ""

    # --- підключення ---------------------------------------------------------
    def available(self) -> bool:
        """Чи запущений RTSS просто зараз (без спроби зайняти слот)."""
        if kernel32 is None:
            return False
        handle = kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, MAP_NAME)
        if not handle:
            return False
        kernel32.CloseHandle(handle)
        return True

    def open(self) -> bool:
        if self.view:
            return True
        if kernel32 is None:
            self.error = "лише Windows"
            return False
        handle = kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, MAP_NAME)
        if not handle:
            self.error = "RTSS не запущено"
            return False
        kernel32.MapViewOfFile.restype = ctypes.c_void_p
        view = kernel32.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0)
        if not view:
            kernel32.CloseHandle(handle)
            self.error = "не вдалося відкрити спільну памʼять RTSS"
            return False
        self.handle, self.view = handle, view

        header = self._read(0, 36)
        signature, version = struct.unpack_from("<II", header, 0)
        if signature != SIGNATURE:
            self.close()
            self.error = "чужа памʼять під іменем RTSS"
            return False
        if version < 0x00020000:
            self.close()
            self.error = "надто стара версія RTSS"
            return False
        (self.osd_entry_size, self.osd_offset,
         self.osd_count) = struct.unpack_from("<III", header, 20)
        self.slot = self._claim_slot()
        if self.slot < 0:
            self.close()
            self.error = "у RTSS немає вільного слоту OSD"
            return False
        self.error = ""
        return True

    def close(self):
        if self.view:
            kernel32.UnmapViewOfFile(ctypes.c_void_p(self.view))
        if self.handle:
            kernel32.CloseHandle(self.handle)
        self.view = self.handle = None

    # --- слоти ---------------------------------------------------------------
    def _slot_at(self, index: int) -> int:
        return self.osd_offset + index * self.osd_entry_size

    def _claim_slot(self) -> int:
        """Свій слот, або перший вільний.

        Нульовий слот пропускаємо навмисно: він належить головному клієнту
        (сам RTSS, MSI Afterburner) — зайняти його означає стерти чужий лічильник
        кадрів.
        """
        free = -1
        for i in range(1, min(self.osd_count, 64)):
            owner = self._read(self._slot_at(i) + OSD_OWNER, 256).split(b"\0", 1)[0]
            if owner.decode("latin-1", "replace") == OWNER:
                return i
            if not owner and free < 0:
                free = i
        return free

    # --- запис ---------------------------------------------------------------
    def show(self, lines) -> bool:
        """Кладе рядки в OSD. Порожній список прибирає наш текст."""
        if not self.open():
            return False
        text = "\n".join(lines)[:4000]
        self._write(self._slot_at(self.slot) + OSD_OWNER, _fixed(OWNER, 256))
        # szOSD — короткий рядок, szOSDEx — довгий; RTSS показує другий, якщо
        # він не порожній, тому основний текст пишемо саме туди.
        self._write(self._slot_at(self.slot) + OSD_TEXT, _fixed("", 256))
        self._write(self._slot_at(self.slot) + OSD_TEXT_EX, _fixed(text, 4096))
        self._bump_frame()
        return True

    def clear(self):
        """Звільняє слот: і текст, і власника — щоб RTSS про нас забув."""
        if not self.view:
            return
        self._write(self._slot_at(self.slot) + OSD_TEXT, _fixed("", 256))
        self._write(self._slot_at(self.slot) + OSD_TEXT_EX, _fixed("", 4096))
        self._write(self._slot_at(self.slot) + OSD_OWNER, _fixed("", 256))
        self._bump_frame()
        self.close()

    def _bump_frame(self):
        """Лічильник кадрів OSD: без нього RTSS не перемалює вже показане."""
        frame = struct.unpack_from("<I", self._read(32, 4), 0)[0]
        self._write(32, struct.pack("<I", (frame + 1) & 0xFFFFFFFF))

    # --- сира памʼять --------------------------------------------------------
    def _read(self, offset: int, size: int) -> bytes:
        buf = (ctypes.c_char * size).from_address(self.view + offset)
        return bytes(buf)

    def _write(self, offset: int, data: bytes):
        ctypes.memmove(self.view + offset, data, len(data))


def _fixed(text: str, size: int) -> bytes:
    """Рядок фіксованої довжини з нулем на кінці.

    RTSS читає це як char[], тож кодуємо в UTF-8 і ріжемо по межі символу:
    обірваний посередині символ показався б сміттям.
    """
    raw = text.encode("utf-8", "replace")[:size - 1]
    while raw and (raw[-1] & 0xC0) == 0x80:
        raw = raw[:-1]
    return raw + b"\0" * (size - len(raw))


# --- дзеркало чату ----------------------------------------------------------

MAX_LINES = 8            # більше в OSD не влазить і читати ніхто не встигає
MAX_CHARS = 90           # довгий рядок RTSS не переносить — ріжемо самі
PLATFORM_MARK = {"twitch": "TW", "kick": "KK", "youtube": "YT", "site": "•"}


class ChatMirror:
    """Останні рядки чату в OSD RTSS.

    Тримає невелику чергу і пише її цілком: RTSS показує СТАН слоту, а не потік
    подій, тож «додати рядок» тут — це «перемалювати останні N».
    """

    def __init__(self):
        self.bridge = Bridge()
        self.enabled = False
        self.lines = []

    def available(self) -> bool:
        return self.bridge.available()

    def set_enabled(self, on: bool) -> bool:
        self.enabled = bool(on)
        if not self.enabled:
            self.bridge.clear()
            return True
        return self.bridge.show(self.lines or ["Hominka: чат зʼявиться тут"])

    def push(self, event: dict):
        """Одна подія чату → один рядок OSD."""
        if not self.enabled:
            return
        kind = event.get("kind")
        if kind not in ("msg", "system"):
            return          # видалення й бани в OSD показувати нічого
        mark = PLATFORM_MARK.get(event.get("platform", ""), "")
        if kind == "system":
            line = "%s %s" % (mark, event.get("text", ""))
        else:
            money = event.get("amount")
            name = event.get("name") or event.get("nick") or "?"
            line = "%s %s: %s%s" % (mark, name, ("%s " % money) if money else "",
                                    event.get("text", ""))
        line = " ".join(line.split())[:MAX_CHARS]
        self.lines.append(line)
        del self.lines[:-MAX_LINES]
        self.bridge.show(self.lines)

    def stop(self):
        self.bridge.clear()
