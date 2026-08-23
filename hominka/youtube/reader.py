"""Читач чату YouTube: окремий потік, події сигналом."""

from threading import Event, Thread

from PySide6.QtCore import QObject, Signal

from .session import LIVE_RECHECK, POLL_DEFAULT, RETRY, find_live_video, open_session, poll

class YouTubeChat(QObject):
    """Читає чат каналу YouTube. Сам знаходить трансляцію і чекає її початку."""

    event = Signal(dict)
    status = Signal(str)

    def __init__(self, channel: str, parent=None):
        super().__init__(parent)
        self.channel = (channel or "").strip()
        self._stop = Event()
        self._thread = None

    def start(self):
        if self._thread or not self.channel:
            return
        self._thread = Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()

    def _run(self):
        while not self._stop.is_set():
            try:
                video = find_live_video(self.channel)
            except Exception as e:
                self.status.emit("YouTube: %s" % e)
                video = ""
            if not video:
                # Ефіру немає — це нормальний стан, а не помилка.
                self._stop.wait(LIVE_RECHECK)
                continue
            try:
                self._read(video)
            except Exception as e:
                self.status.emit("YouTube: %s" % e)
                self._stop.wait(RETRY)

    def _read(self, video: str):
        key, ver, cont = open_session(video)
        if not cont:
            self._stop.wait(RETRY)
            return
        self.status.emit("")
        first = True
        while not self._stop.is_set() and cont:
            events, cont, pause = poll(key, ver, cont)
            # Перша пачка — це історія чату; сипати нею в стрічку ні до чого.
            if not first:
                for e in events:
                    self.event.emit(e)
            first = False
            self._stop.wait(pause or POLL_DEFAULT)
