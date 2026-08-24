"""Чат у грі: рендеримо його поза екраном і кладемо кадр у спільну память.

Важке — сокети, розбір, шрифти, емоути — лишається тут, у нашому процесі. У грі
(через native/overlay.dll) живе лише «взяти готову картинку і намалювати». Впаде
чат — гра не помітить: DLL малюватиме останній кадр.

Домовленість про формат — native/common/shared_frame.h. Тут її дзеркало на
Python; якщо міняється там, міняється й тут (обидва звіряються з версією).

Чому не QWebEngineView.grab() «offscreen»: Chromium не малює вікно, якого не
видно, і grab() віддає порожнечу. Тому вікно РЕАЛЬНО показуємо — але за межами
робочого столу (координата далеко в мінусі): воно малюється, ми його знімаємо, а
на екрані його немає. Перевірено: і так знятий кадр має справжню альфу.
"""

import ctypes
import os
import struct
import zlib
from ctypes import wintypes

from PySide6.QtCore import Qt, QTimer, QObject
from PySide6.QtGui import QImage
from PySide6.QtWebEngineWidgets import QWebEngineView

from . import feed as chatfeed


def _diag(msg: str):
    """Дописує рядок у той самий журнал, що й DLL (%TEMP%\\hominka-overlay.log).

    Продюсер (ми) і DLL пишуть в одне місце — тоді видно обидва боки: чи ми
    взагалі щось поклали в память і що саме DLL звідти прочитала.
    """
    try:
        path = os.path.join(os.environ.get("TEMP", "."), "hominka-overlay.log")
        from datetime import datetime
        with open(path, "a", encoding="utf-8") as f:
            f.write("[%s] продюсер: %s\n" % (datetime.now().strftime("%H:%M:%S.%f")[:-3], msg))
    except Exception:
        pass

# --- дзеркало shared_frame.h ------------------------------------------------
SHM_NAME = "Local\\HominkaOverlayFrame"
MAGIC = 0x324B4D48          # «HMK2» у памʼяті
VERSION = 2
MAX_W, MAX_H = 1920, 1080
HEADER_SIZE = 64
DATA_MAX = MAX_W * MAX_H * 4
TOTAL = HEADER_SIZE + DATA_MAX

ANCHOR = {"tl": 0, "tr": 1, "bl": 2, "br": 3}

_IS_WINDOWS = hasattr(ctypes, "windll")


class SharedFrameWriter:
    """Іменований мапінг у пейджфайл + запис кадру через seqlock.

    Пишемо через ctypes напряму, а не QSharedMemory: так контролюємо і саме
    ім'я (Local\\…, як чекає DLL), і формат заголовка байт у байт.
    """

    def __init__(self):
        self._h = None
        self._view = None
        self._mutex = None
        self._seq = 0
        self.target_pid = 0     # 0 = будь-який процес; ставить set_target()
        self.conflict = False
        if not _IS_WINDOWS:
            return
        k32 = ctypes.windll.kernel32
        k32.CreateFileMappingW.restype = wintypes.HANDLE
        k32.CreateFileMappingW.argtypes = [
            wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
            wintypes.DWORD, wintypes.DWORD, wintypes.LPCWSTR]
        k32.CreateMutexW.restype = wintypes.HANDLE
        k32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
        k32.MapViewOfFile.restype = wintypes.LPVOID
        k32.MapViewOfFile.argtypes = [
            wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
            wintypes.DWORD, ctypes.c_size_t]
        # Без argtypes ctypes бере вказівник за c_int і переповнюється на
        # адресах > 2 ГБ — саме це валило close().
        k32.UnmapViewOfFile.argtypes = [wintypes.LPVOID]
        k32.CloseHandle.argtypes = [wintypes.HANDLE]

        # «Інший продюсер вже пише» ловимо ІМЕНОВАНИМ МЮТЕКСОМ, а не тим, що
        # мапінг уже існує. Мапінг тримає живим і ЧИТАЧ — наша ж DLL у будь-якій
        # грі, куди її вклали. Тобто поки хоч одна гра з оверлеєм відкрита,
        # CreateFileMapping завжди каже «вже існує», і перевірка через це
        # помилково вважала б конфліктом навіть єдину копію Hominka (саме через
        # це чат і не малювався). Мютекс же чіпає лише продюсер.
        ERROR_ALREADY_EXISTS = 183
        self._mutex = k32.CreateMutexW(None, False, "Local\\HominkaOverlayProducer")
        if self._mutex and k32.GetLastError() == ERROR_ALREADY_EXISTS:
            self.conflict = True
            k32.CloseHandle(self._mutex)
            self._mutex = None
            return

        INVALID = wintypes.HANDLE(-1)
        PAGE_READWRITE = 0x04
        # Мапінг може вже існувати через читача — це нормально: ми відкриваємо
        # ТУ САМУ памʼять і пишемо в неї, DLL читає наші кадри.
        self._h = k32.CreateFileMappingW(INVALID, None, PAGE_READWRITE,
                                         0, TOTAL, SHM_NAME)
        if not self._h:
            return
        FILE_MAP_ALL_ACCESS = 0xF001F
        self._view = k32.MapViewOfFile(self._h, FILE_MAP_ALL_ACCESS, 0, 0, TOTAL)
        if self._view:
            # Порожній заголовок: enabled=0, поки нема першого кадру.
            ctypes.memset(self._view, 0, HEADER_SIZE)

    def ok(self) -> bool:
        return bool(self._view)

    def set_target(self, pid: int):
        """Процес, у якому дозволено малювати чат (0 = будь-який)."""
        self.target_pid = int(pid) & 0xFFFFFFFF
        if self._view:
            self._begin()
            struct.pack_into("<I", self._buf, 48, self.target_pid)
            self._commit_header()

    def set_enabled(self, on: bool):
        """Показати/сховати чат у грі, не чіпаючи самого кадру."""
        if not self._view:
            return
        # enabled — 11-те 32-бітне поле (зсув 40). Пишемо його окремо, під
        # seqlock, щоб DLL не побачив півстану.
        self._begin()
        struct.pack_into("<I", self._buf, 40, 1 if on else 0)
        self._commit_header()

    # --- внутрішнє ---
    def _begin(self):
        # Тримаємо локальну копію заголовка, щоб не читати з мапінгу побайтово.
        if not hasattr(self, "_buf"):
            self._buf = bytearray(HEADER_SIZE)
        self._seq |= 1  # непарне = запис триває
        struct.pack_into("<I", self._buf, 8, self._seq)
        ctypes.memmove(self._view, bytes(self._buf[:12]), 12)

    def _commit_header(self):
        self._seq = (self._seq + 1) & 0xFFFFFFFF
        if self._seq & 1:
            self._seq = (self._seq + 1) & 0xFFFFFFFF
        struct.pack_into("<I", self._buf, 8, self._seq)
        ctypes.memmove(self._view, bytes(self._buf), HEADER_SIZE)

    def write(self, img: QImage, anchor: int, margin_x: int, margin_y: int,
              opacity: int, heartbeat: int):
        """Кладе кадр (QImage ARGB32) у память під seqlock."""
        if not self._view:
            return
        w, h = img.width(), img.height()
        if w == 0 or h == 0:
            return
        stride = w * 4
        data_len = stride * h
        if data_len > DATA_MAX:
            return

        buf = bytearray(HEADER_SIZE)
        struct.pack_into("<IIIIIIIiiIIII", buf, 0,
                         MAGIC, VERSION, 0,           # seq заповнимо навколо запису
                         w, h, stride, anchor,
                         margin_x, margin_y, opacity & 0xFF, 1, heartbeat & 0xFFFFFFFF,
                         self.target_pid & 0xFFFFFFFF)
        self._buf = buf

        # seqlock: непарне → дані → заголовок з парним seq.
        self._seq |= 1
        struct.pack_into("<I", buf, 8, self._seq)
        ctypes.memmove(self._view, bytes(buf[:12]), 12)

        # Пікселі рядок за рядком (bytesPerLine може мати вирівнювання).
        base = ctypes.cast(self._view, ctypes.c_void_p).value + HEADER_SIZE
        bpl = img.bytesPerLine()
        bits = img.constBits()               # memoryview на всі байти QImage
        mv = memoryview(bits)
        for y in range(h):
            row = mv[y * bpl: y * bpl + stride]
            ctypes.memmove(base + y * stride, bytes(row), stride)

        self._commit_header()

    def close(self):
        if not _IS_WINDOWS:
            return
        k32 = ctypes.windll.kernel32
        if self._view:
            k32.UnmapViewOfFile(self._view)
            self._view = None
        if self._h:
            k32.CloseHandle(self._h)
            self._h = None
        if self._mutex:
            k32.CloseHandle(self._mutex)   # звільняє «продюсер активний»
            self._mutex = None


class GameOverlay(QObject):
    """Малює той самий чат, що й головне вікно, у власне приховане вікно і
    щокадру-за-потребою кладе його в спільну память для гри.

    Живе своїм життям поруч із головним вікном: ту саму подію отримує і воно, і
    ми (тее в Overlay._on_chat_event). Свій CSS і порядок частин беремо з
    головного вікна, щоб у грі чат виглядав так само.
    """

    IDLE_MS = 1000          # коли нічого не відбувається — знімаємо рідко
    BUSY_MS = 180           # після події деякий час знімаємо часто (анімація появи)
    BUSY_WINDOW = 2.5       # скільки секунд лишатися «активними» після події

    def __init__(self, win):
        # Без батька-QObject: час життя нам задає сам Overlay (кличе close()),
        # а вимога, щоб win був QObject, лише заважала б і тестам, і майбутнім
        # викликам.
        super().__init__()
        self.win = win
        self.enabled = False
        self.anchor = ANCHOR["tl"]
        self.margin_x = 24
        self.margin_y = 24
        self.opacity = 255
        self._heartbeat = 0
        self._last_crc = 0
        self._busy_until = 0.0
        self._in_tick = False
        self._pushed = 0
        self._content_logged = False

        self.writer = SharedFrameWriter()

        # Приховане вікно за межами робочого столу — Chromium малює лише те, що
        # «показане». WA_TranslucentBackground + прозорий фон сторінки дають
        # альфу; Qt.Tool прибирає його з панелі завдань і Alt-Tab.
        self.view = QWebEngineView()
        self.view.setWindowFlags(Qt.Tool | Qt.FramelessWindowHint |
                                 Qt.WindowDoesNotAcceptFocus)
        self.view.setAttribute(Qt.WA_TranslucentBackground, True)
        self.view.setAttribute(Qt.WA_ShowWithoutActivating, True)
        self.view.page().setBackgroundColor(Qt.transparent)
        self.view.resize(360, 560)

        self.feed = chatfeed.ChatFeed(self.view)
        self.feed.custom_css = getattr(win, "custom_css", "") or ""
        self.feed.layout = list(getattr(win, "chat_layout", None) or chatfeed.DEFAULT_LAYOUT)
        self.view.loadFinished.connect(self._on_loaded)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)

    # --- керування ззовні ---
    def set_enabled(self, on: bool):
        self.enabled = bool(on)
        if not self.writer.ok():
            _diag("увімкнення не вдалося — спільна память недоступна (conflict=%s)"
                  % getattr(self.writer, "conflict", "?"))
            return
        if self.enabled:
            self.view.move(-4000, -4000)
            self.view.show()
            self.feed.load()
            self._busy_until = _now() + self.BUSY_WINDOW
            self._timer.start(self.BUSY_MS)
            _diag("увімкнено, розмір вікна %dx%d" % (self.view.width(), self.view.height()))
        else:
            # Спершу глушимо таймер — щоб жоден відкладений _tick не записав
            # кадр із enabled=1 уже ПІСЛЯ того, як ми вимкнули чат.
            self._timer.stop()
            self.writer.set_enabled(False)
            self.view.hide()

    def set_geometry(self, anchor: str, margin_x: int, margin_y: int, opacity: int):
        self.anchor = ANCHOR.get(anchor, 0)
        self.margin_x = int(margin_x)
        self.margin_y = int(margin_y)
        self.opacity = max(0, min(255, int(opacity)))
        self._last_crc = 0    # змусити перезапис із новими полями

    def set_size(self, w: int, h: int):
        self.view.resize(max(80, min(MAX_W, w)), max(60, min(MAX_H, h)))
        self._last_crc = 0

    def set_target(self, pid: int):
        """Малювати чат лише у цій грі (0 = будь-де). Ставиться після інʼєкції."""
        self.writer.set_target(pid)
        _diag("ціль pid=%d" % pid)

    def set_custom_css(self, css: str):
        self.feed.set_custom_css(css or "")
        self._wake()

    def set_layout(self, layout):
        self.feed.set_layout(layout)
        self._wake()

    def push(self, event: dict):
        """Та сама подія, що пішла в головне вікно."""
        if not self.enabled:
            return
        self._pushed += 1
        if self._pushed == 1:
            _diag("перша подія чату дійшла до оверлея гри")
        self.feed.push(event)
        self._wake()

    def close(self):
        self._timer.stop()
        self.writer.close()
        self.view.deleteLater()

    # --- внутрішнє ---
    def _on_loaded(self, ok: bool):
        if ok:
            self.feed.on_loaded()

    def _wake(self):
        """Була подія — знімаємо частіше найближчі кілька секунд."""
        if not self.enabled:
            return
        self._busy_until = _now() + self.BUSY_WINDOW
        if self._timer.interval() != self.BUSY_MS:
            self._timer.start(self.BUSY_MS)

    def _tick(self):
        if not self.enabled or not self.writer.ok():
            return
        # grab() прокручує цикл подій усередині себе, і таймер міг би влізти в
        # _tick рекурсивно — звідси зайві сотні записів. Обороняємось прапорцем.
        if self._in_tick:
            return
        self._in_tick = True
        try:
            self._grab_and_write()
        finally:
            self._in_tick = False

    def _grab_and_write(self):
        # Знімок прихованого вікна → ARGB32 (у памʼяті це BGRA, як чекає DLL).
        img = self.view.grab().toImage().convertToFormat(QImage.Format_ARGB32)
        if img.width() > MAX_W or img.height() > MAX_H:
            img = img.scaled(min(img.width(), MAX_W), min(img.height(), MAX_H),
                             Qt.KeepAspectRatio, Qt.SmoothTransformation)

        crc = zlib.crc32(bytes(memoryview(img.constBits())))
        crc ^= (self.anchor << 24) ^ (self.opacity << 16) ^ \
               ((self.margin_x & 0xFF) << 8) ^ (self.margin_y & 0xFF)
        if crc != self._last_crc:
            self._last_crc = crc
            self._heartbeat += 1
            self.writer.write(img, self.anchor, self.margin_x, self.margin_y,
                              self.opacity, self._heartbeat)
            # Коли вже прийшли події — рахуємо непорожні пікселі кадру: так видно,
            # чи офскрин-вікно справді намалювало чат, чи віддає прозору пустку
            # (тоді проблема в рендері вікна, а не в подіях). Перший кадр не
            # рахуємо — він порожній за визначенням (сторінка щойно завантажилась).
            if not self._content_logged and self._pushed > 0:
                self._content_logged = True
                nb = 0
                step = max(1, img.width() // 90)
                for y in range(0, img.height(), 6):
                    for x in range(0, img.width(), step):
                        c = img.pixelColor(x, y)
                        if c.alpha() > 10 and (c.red() + c.green() + c.blue()) > 20:
                            nb += 1
                _diag("кадр після подій %dx%d, непорожніх пікселів (вибірка)=%d, target=%d, heartbeat=%d"
                      % (img.width(), img.height(), nb, self.writer.target_pid, self._heartbeat))

        # Тихо стало — переходимо на рідкі знімки, щоб не молоти вхолосту.
        if _now() > self._busy_until and self._timer.interval() != self.IDLE_MS:
            self._timer.start(self.IDLE_MS)


def _now() -> float:
    import time
    return time.monotonic()
