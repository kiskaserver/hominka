"""Пошук власного ефіру на YouTube — без браузера.

Те саме, що робить probe.py, але звичайним запитом замість прихованої сторінки
Chromium. Причина проста: коли чат малює нативний рендер, браузера в програмі
немає взагалі, і піднімати його заради одного HTTP-запиту раз на кілька хвилин
було б безглуздо — саме через це «нуль процесів Chromium» і не виходило.

Нічого нового тут не винаходиться: сторінки YouTube уже вміє читати
hominka/youtube (той самий модуль, яким читається сам чат), і потрібне береться
звідти.

Обмеження чесно: запит анонімний, тож ефір «тільки для учасників» цей шлях не
побачить. Для власного публічного ефіру — а це те, заради чого поле й існує, —
різниці немає.
"""

import threading

from PySide6.QtCore import QObject, Signal

from .urls import channel_id_from, channel_page_url
from .youtube.net import extract_json, jget
from .youtube.session import find_live_video


class LiveProbeLight(QObject):
    """Той самий інтерфейс, що й у probe.LiveProbe: busy, start(), result."""

    result = Signal(dict)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.busy = False

    def start(self, my_channel: str, known_id: str):
        if self.busy:
            return
        self.busy = True
        threading.Thread(target=self._work, args=(my_channel or "", known_id or ""),
                         daemon=True).start()

    def _work(self, my_channel: str, known_id: str):
        info = {"channelId": known_id, "title": "", "video": ""}
        try:
            info.update(self._lookup(my_channel, known_id))
        except Exception:
            # Мережа може не відповісти, YouTube — змінити розмітку. Це не
            # привід ані падати, ані сигналити про неіснуючий ефір.
            pass
        self.busy = False
        self.result.emit(info)

    @staticmethod
    def _lookup(my_channel: str, known_id: str) -> dict:
        out = {}
        # id каналу: або він уже відомий, або його треба дістати зі сторінки —
        # короткого способу перетворити «@нік» на UC-id у YouTube немає.
        cid = channel_id_from(my_channel) or known_id
        title = ""
        if my_channel.strip():
            from .youtube.net import _get
            html = _get(channel_page_url(my_channel))
            data = extract_json(html, "ytInitialData")
            meta = jget(data, "metadata", "channelMetadataRenderer") or {}
            cid = meta.get("externalId") or cid
            title = meta.get("title") or ""
        if cid:
            out["channelId"] = cid
        if title:
            out["title"] = title

        target = my_channel.strip() or cid
        if target:
            out["video"] = find_live_video(target) or ""
        return out
