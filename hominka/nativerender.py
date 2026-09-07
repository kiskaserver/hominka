"""Керування нативним рендером чату (native/hominka-render-x64.exe).

Що це замість чого. Раніше «чат поверх гри» працював так: браузер малює чат у
вікні, Python знімає з нього кадр (grab), рахує CRC по всьому кадру, кладе
пікселі у спільну памʼять, а окремий процес показує їх через
DirectComposition. Тобто кадр проходив коло GPU→CPU→GPU, і саме звідси
бралися і стеля в ~8 кадрів на секунду, і пауза грабера під час перетягування
вікна.

Тепер той самий процес малює чат САМ. Сюди йдуть не пікселі, а події: «нове
повідомлення», «прибери це», «ось свій CSS», «ось картинка емоута». Малює він
через litehtml + Direct2D прямо в текстуру, яку показує DirectComposition —
жодного копіювання і жодного грабера.

У гру, як і раніше, НІЧОГО не вкладається — безпечно для античитів.

Домовленість про кадр описана в native/render/ipc.h; тут її дзеркало.
"""

import json
import os
import queue
import socket
import struct
import subprocess
import sys
import threading
import ctypes
# wintypes існує лише на Windows: там, де його немає, сам імпорт кидає
# помилку, і модуль не завантажився б узагалі. Усе, що ним користується, і так
# під «if IS_WINDOWS».
if sys.platform == "win32":
    from ctypes import wintypes
else:
    wintypes = None

from PySide6.QtCore import QObject, Signal

from .inject import native_dir

IS_WINDOWS = sys.platform == "win32"

# Ім'я бінаря й спосіб зв'язку різні, а домовленість про кадри — та сама
# (див. native/render/ipc.h). Тому нижче різниться рівно транспорт.
_EXE = "hominka-render-x64.exe" if IS_WINDOWS else "hominka-render-linux"
# Клас вікна, яке робить нативний процес. Шукаємо вікно саме за ним, щоб
# поставити його туди ж, де вікно чату.
_WND_CLASS = "HominkaRenderOverlay"

if IS_WINDOWS:
    # Без argtypes/restype ctypes бере HWND за c_int і ОБРІЗАЄ 64-бітний
    # вказівник — на цьому вже горіли в dcomp.py: SetWindowPos рухав не те
    # вікно, і оверлей застрягав у лівому куті. Оголошуємо типи явно.
    _u32 = ctypes.windll.user32
    _u32.FindWindowW.restype = wintypes.HWND
    _u32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    _u32.SetWindowPos.restype = wintypes.BOOL
    _u32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
    _u32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]

_HWND_TOPMOST = wintypes.HWND(-1) if IS_WINDOWS else None
_SWP_NOSIZE = 0x0001
_SWP_NOACTIVATE = 0x0010
_SWP_SHOWWINDOW = 0x0040
_SW_HIDE = 0


def _channel(suffix: str, back: bool) -> str:
    """Ім'я каналу. Під Windows — іменований канал, поза ним — файл сокета.

    Обидва містять наш PID: два запущені екземпляри Hominka не мають битися
    за один канал.
    """
    tail = "-back" if back else ""
    if IS_WINDOWS:
        return r"\\.\pipe\Hominka-%d%s%s" % (os.getpid(), suffix, tail)
    # XDG_RUNTIME_DIR — тека саме цього користувача й сеансу; це найближче до
    # простору імен Local\ у Windows. Немає її — /tmp.
    base = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    # Саме через «/», а не os.path.join: це шлях сокета POSIX, і збирати його
    # роздільником поточної системи неправильно.
    return "%s/hominka-%d%s%s.sock" % (base.rstrip("/"), os.getpid(), suffix, tail)


class _SockFile:
    """Сокет у вигляді файла: write/read/close — і більше нічого.

    Потрібен, щоб решта коду не знала, з чим має справу. Під Windows там
    звичайний open() іменованого каналу, тут — AF_UNIX, а користуються ними
    однаково.
    """

    def __init__(self, sock):
        self._s = sock

    def write(self, data: bytes):
        self._s.sendall(data)

    def read(self, n: int) -> bytes:
        return self._s.recv(n)

    def close(self):
        try:
            self._s.close()
        except Exception:
            pass


def _connect(name: str, write: bool):
    """Відкриває канал. OSError — ще не піднявся, спробуємо пізніше."""
    if IS_WINDOWS:
        return open(name, "wb" if write else "rb", buffering=0)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(name)
    except OSError:
        sock.close()
        raise
    return _SockFile(sock)


def _diag(msg: str):
    """У той самий журнал, що й решта оверлея — щоб дивитися в одному місці."""
    from .diag import log
    log("рендер: " + msg)


class NativeRenderer(QObject):
    """Процес рендера + канал до нього.

    Писати можна з будь-якого потоку: черга й окремий потік-писар для того й
    існують. Головне — щоб потік Qt ніколи не чекав на канал: якщо рендер
    забарився, чат у вікні має жити далі.

    Назад приходить те, що людина зробила у вікні оверлея: покрутила
    прозорість, перетягнула, натиснула шестерню. Віддаємо це сигналом — Qt сам
    перекине його з потоку-читача в потік вікна, і нам не треба ані блокувань,
    ані черг «на той бік».
    """

    # Подія від рендера: словник із полем «t» (look / lock / geometry / settings).
    event = Signal(dict)

    # Кадр предпросмотру: (ширина, висота, пікселі BGRA premultiplied).
    # Окремим сигналом, а не через event: пікселі не варто ганяти крізь той
    # самий розбір, що й короткі повідомлення.
    frame = Signal(int, int, bytes)

    def __init__(self, parent=None, preview: bool = False):
        super().__init__(parent)
        # Предпросмотр — окремий процес із окремим каналом. Він не має нічого
        # спільного з оверлеєм на екрані: ані вікна, ані DirectComposition, і
        # чіпати те, що бачить глядач, він не може за побудовою.
        self._preview = bool(preview)
        self._suffix = "-preview" if preview else ""
        self._proc = None
        self._hwnd = 0
        self._pipe = None
        self._queue = queue.Queue(maxsize=4096)
        self._writer = None
        self._reader = None
        self._stop = threading.Event()
        # Що вже надіслано: не женемо ту саму картинку вдруге, навіть якщо
        # емоут трапляється в кожному другому рядку.
        self._sent_images = set()
        # Останній надісланий вигляд — щоб не слати те саме щотакту (див.
        # set_config).
        self._cfg = {}

    # --- життєвий цикл ------------------------------------------------------

    def available(self) -> bool:
        return os.path.isfile(os.path.join(native_dir(), _EXE))

    def alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def start(self) -> bool:
        if not self.available():
            _diag("немає %s" % _EXE)
            return False
        if self.alive():
            return True
        exe = os.path.join(native_dir(), _EXE)
        try:
            # CREATE_NO_WINDOW — без консолі; процес сам робить своє topmost-вікно.
            # Передаємо свій PID: він стежить за нами й вийде, якщо Hominka
            # зникне (навіть при аварійному завершенні) — без сиріт на екрані.
            mode = "--preview" if self._preview else "--run"
            # CREATE_NO_WINDOW — щоб не блимала консоль; поза Windows такого
            # прапорця немає й не треба.
            extra = {"creationflags": 0x08000000} if IS_WINDOWS else {}
            self._proc = subprocess.Popen([exe, mode, str(os.getpid())], **extra)
        except OSError as e:
            _diag("не запустився: %r" % (e,))
            self._proc = None
            return False
        self._hwnd = 0
        self._sent_images.clear()
        self._cfg.clear()          # новий процес нічого про нас не знає
        self._stop.clear()
        self._writer = threading.Thread(target=self._write_loop, daemon=True)
        self._writer.start()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        return True

    def stop(self):
        if self._proc is not None:
            try:
                self.send("bye")
            except Exception:
                pass
        self._stop.set()
        try:
            self._queue.put_nowait(None)
        except queue.Full:
            pass
        if self._proc is not None:
            try:
                self._proc.wait(timeout=1.0)
            except Exception:
                try:
                    self._proc.kill()
                except Exception:
                    pass
            self._proc = None
        self._close_pipe()
        self._hwnd = 0

    # --- розташування -------------------------------------------------------

    def place(self, left: int, top: int):
        """Ставить вікно рендера туди, де на екрані лежить чат.

        Розмір веде сам процес (за тим, що ми надіслали в «config»), тож тут
        лише позиція — SWP_NOSIZE.
        """
        if not self.alive():
            return
        if not IS_WINDOWS:
            # Поза Windows позицію везе той самий кадр «config», що й розмір:
            # вікно там override-redirect, тобто ставить себе саме, а лізти в
            # чуже вікно ззовні (як SetWindowPos) на X11 нема чим.
            self.set_config(x=int(left), y=int(top))
            return
        if not self._hwnd:
            self._hwnd = _u32.FindWindowW(_WND_CLASS, None) or 0
            if not self._hwnd:
                return
        _u32.SetWindowPos(self._hwnd, _HWND_TOPMOST, int(left), int(top), 0, 0,
                          _SWP_NOSIZE | _SWP_NOACTIVATE | _SWP_SHOWWINDOW)

    def hide(self):
        if IS_WINDOWS and self._hwnd:
            _u32.ShowWindow(self._hwnd, _SW_HIDE)

    # --- надсилання ---------------------------------------------------------

    def send(self, t: str, blob: bytes = b"", **fields):
        """Кладе кадр у чергу. Ніколи не блокує потік Qt.

        Тип кадру зветься «t», а не «kind», навмисно: у самому повідомленні
        чату вже є своє поле kind ("msg"/"system"), і воно йде в цей же словник
        разом з рештою (chatsources.message). Однакові імена зіштовхнулися б.
        """
        if self._proc is None:
            return
        fields["t"] = t
        try:
            self._queue.put_nowait((json.dumps(fields, ensure_ascii=False), blob))
        except queue.Full:
            # Черга переповнена — рендер завис або не встигає. Викидаємо кадр:
            # чат на секунду відстане, але програма не стане.
            _diag("черга переповнена, кадр %s викинуто" % t)

    def message(self, ev: dict):
        """Повідомлення чату. Приймає рівно те, що будує chatsources.message()."""
        self.send("msg", **ev)

    def delete(self, msg_id: str):
        self.send("delete", id=msg_id)

    def purge(self, nick: str):
        self.send("purge", nick=nick)

    def clear(self):
        self.send("clear")

    def set_css(self, css: str):
        self.send("css", css=css or "")

    def set_layout(self, layout):
        self.send("layout", layout=list(layout or []))

    def set_config(self, zoom: float = None, width: int = None, height: int = None,
                   opacity: float = None, bg_alpha: float = None,
                   frameless: bool = None, locked: bool = None,
                   x: int = None, y: int = None):
        """Вигляд: кегль і розмір. Шлемо ЛИШЕ те, що справді змінилося.

        Це не заощадження на дрібницях: розташування вікна перевіряється кожні
        350 мс, і якби ми щоразу слали config, рендер щоразу вважав би кадр
        зміненим і робив Present. А весь сенс нового шляху в тому, що коли в
        чаті нічого не сталося, відеокарту ніхто не чіпає.
        """
        f = {}
        if zoom is not None and zoom != self._cfg.get("zoom"):
            f["zoom"] = float(zoom)
        if width is not None and width != self._cfg.get("width"):
            f["width"] = int(width)
        if height is not None and height != self._cfg.get("height"):
            f["height"] = int(height)
        if opacity is not None and opacity != self._cfg.get("opacity"):
            f["opacity"] = float(opacity)
        if bg_alpha is not None and bg_alpha != self._cfg.get("bg_alpha"):
            f["bg_alpha"] = float(bg_alpha)
        if frameless is not None and frameless != self._cfg.get("frameless"):
            f["frameless"] = bool(frameless)
        if locked is not None and locked != self._cfg.get("locked"):
            f["locked"] = bool(locked)
        # Позиція їде сюди лише поза Windows (див. place): там вікно ставить
        # себе саме, бо ним не керує віконний менеджер.
        if x is not None and x != self._cfg.get("x"):
            f["x"] = int(x)
        if y is not None and y != self._cfg.get("y"):
            f["y"] = int(y)
        if not f:
            return
        self._cfg.update(f)
        self.send("config", **f)

    def set_enabled(self, on: bool):
        self.send("enabled", on=bool(on))

    def set_inject(self, on: bool, pid: int = 0, opacity: int = 235,
                   hide_obs: bool = False):
        """Кадр для оверлея, ВКЛАДЕНОГО в гру.

        Поки on=False, рендер узагалі не читає кадр із відеокарти — а це
        єдине дороге, що є на цьому шляху.
        """
        self.send("inject", on=bool(on), pid=int(pid or 0),
                  opacity=int(opacity), hide_obs=bool(hide_obs))

    def image(self, url: str, data: bytes):
        """Байти картинки. Ту саму адресу вдруге не шлемо."""
        if not url or not data or url in self._sent_images:
            return
        self._sent_images.add(url)
        self.send("image", blob=data, url=url)

    # --- потік-писар --------------------------------------------------------

    def _open_pipe(self) -> bool:
        if self._pipe is not None:
            return True
        name = _channel(self._suffix, back=False)
        try:
            # Канал створює нативний процес, і на момент першого запису він міг
            # ще не встигнути. Помилку тут не вважаємо бідою: спробуємо на
            # наступному кадрі.
            self._pipe = _connect(name, write=True)
            _diag("канал відкрито")
            return True
        except OSError:
            self._pipe = None
            return False

    def _close_pipe(self):
        if self._pipe is not None:
            try:
                self._pipe.close()
            except Exception:
                pass
            self._pipe = None

    def _read_loop(self):
        """Читає те, що шле рендер: дії людини у вікні.

        ОКРЕМИЙ канал, а не той самий. Спершу був один двосторонній, але
        рантайм C на Windows бере замок на файловий дескриптор: потік-читач
        стоїть у ReadFile, тримає замок, і потік-писар не може написати нічого.
        Два дескриптори — два замки, і ніхто нікого не тримає.

        Формат той самий, що й у другий бік (див. ipc.h): довжина заголовка,
        заголовок, довжина вкладення. Вкладень у цей бік не буває.
        """
        name = _channel(self._suffix, back=True)
        back = None
        buf = b""
        while not self._stop.is_set():
            if back is None:
                try:
                    back = _connect(name, write=False)
                except OSError:
                    self._stop.wait(0.1)
                    continue
                _diag("зворотний канал відкрито")
            try:
                chunk = back.read(4096)
            except OSError:
                try:
                    back.close()
                except Exception:
                    pass
                back = None
                self._stop.wait(0.2)
                continue
            if not chunk:
                self._stop.wait(0.05)
                continue
            buf += chunk
            while len(buf) >= 8:
                (hlen,) = struct.unpack("<I", buf[:4])
                if hlen > 1 << 20:              # сміття — рвемо накопичене
                    buf = b""
                    break
                if len(buf) < 4 + hlen + 4:
                    break
                (blen,) = struct.unpack("<I", buf[4 + hlen:8 + hlen])
                total = 8 + hlen + blen
                if len(buf) < total:
                    break
                head = buf[4:4 + hlen]
                blob = buf[8 + hlen:total] if blen else b""
                buf = buf[total:]
                try:
                    ev = json.loads(head.decode("utf-8"))
                except (ValueError, UnicodeDecodeError):
                    continue
                if ev.get("t") == "frame":
                    self.frame.emit(int(ev.get("w", 0)), int(ev.get("h", 0)), blob)
                else:
                    self.event.emit(ev)

    def _write_loop(self):
        pending = None
        while not self._stop.is_set():
            item = pending
            pending = None
            if item is None:
                try:
                    item = self._queue.get(timeout=0.25)
                except queue.Empty:
                    continue
            if item is None:
                return
            if not self._open_pipe():
                # Рендер ще не піднявся. Кадр не викидаємо — він може бути
                # важливим (css/config), і чекати тут дешевше, ніж губити.
                pending = item
                self._stop.wait(0.1)
                continue
            head, blob = item
            data = head.encode("utf-8")
            try:
                # «довжина заголовка + заголовок + довжина вкладення + вкладення» —
                # межі кадрів задають довжини, а не роздільник: у вкладенні
                # двійкові байти картинки, там роздільник трапиться неодмінно.
                self._pipe.write(struct.pack("<I", len(data)) + data +
                                 struct.pack("<I", len(blob)) + blob)
            except OSError:
                # Рендер відвалився (упав або його закрили). Канал перевідкриємо
                # на наступному кадрі; програма через це падати не має.
                _diag("запис у канал не вдався — перевідкриваю")
                self._close_pipe()
                self._stop.wait(0.2)
