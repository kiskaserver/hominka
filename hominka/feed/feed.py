"""Черга подій до сторінки: затримка чату і рівний темп подачі."""

import json
import time

from PySide6.QtCore import QObject, QTimer, QUrl

from .page import DEFAULT_LAYOUT, apply_css_js, apply_layout_js, page_html

# Мінімальний проміжок між рядками, коли черга розсмоктується. Саме він і
# рятує від «каші»: навіть якщо площадки віддали двадцять повідомлень за раз,
# на екран вони виходять по одному, а не стіною.
PACE_MS = 130


class ChatFeed(QObject):
    """Приймає події читачів і малює їх на сторінці у вікні чату.

    Уміє тримати повідомлення на затримці. Затримка потрібна з двох причин:
    підігнати чат під затримку самої трансляції і — головне — встигати читати,
    коли пишуть швидше, ніж людина встигає дивитися.
    """

    def __init__(self, view, parent=None):
        super().__init__(parent)
        self.view = view
        self.custom_css = ""
        # Порядок частин рядка (див. page.PARTS). Список, а не рядок: його
        # переставляють у ⚙ → свій CSS → «Порядок».
        self.layout = list(DEFAULT_LAYOUT)
        self.ready = False
        self._queue = []          # чекають завантаження сторінки
        self._delayed = []        # (коли показати, подія)
        self.delay = 0.0
        self._timer = QTimer(self)
        self._timer.setInterval(PACE_MS)
        self._timer.timeout.connect(self._flush)

    def set_delay(self, seconds: float):
        self.delay = max(0.0, float(seconds or 0))
        if not self.delay:
            # Вимкнули затримку — все, що чекало, показуємо одразу, інакше
            # воно зависло б назавжди.
            pending, self._delayed = self._delayed, []
            for _due, e in pending:
                self._render(e)
            self._timer.stop()

    def load(self):
        """Показує порожню стрічку. Базова адреса потрібна, щоб браузер пускав
        картинки емоутів із чужих доменів."""
        self.ready = False
        self._queue = []
        self._delayed = []
        self.view.setHtml(page_html(self.custom_css, self.layout),
                          QUrl("https://stream.svitix.com/"))

    def set_custom_css(self, css: str):
        """Новий свій CSS — одразу на екран, не чекаючи перезавантаження."""
        self.custom_css = css or ""
        if self.ready:
            self.view.page().runJavaScript(apply_css_js(self.custom_css))

    def set_layout(self, layout):
        """Новий порядок частин — так само одразу, без перезавантаження.

        Перезавантажити було б простіше, але воно змело б увесь чат, який
        зараз на екрані, — а порядок підбирають саме дивлячись на живі рядки.
        """
        self.layout = list(layout or DEFAULT_LAYOUT)
        if self.ready:
            self.view.page().runJavaScript(apply_layout_js(self.layout))

    def on_loaded(self):
        self.ready = True
        pending, self._queue = self._queue, []
        for e in pending:
            self.push(e)

    def push(self, event: dict):
        """Подія → сторінка. До завантаження складаємо в чергу, інакше перші
        повідомлення (а вони приходять одразу) просто зникли б."""
        # Від відповіді лишилося саме звертання (див. trim_reply_mention) —
        # показувати порожній рядок з ніком нема сенсу.
        if event.get("kind") == "msg" and not event.get("text") and not event.get("amount"):
            return
        if not self.ready:
            self._queue.append(event)
            if len(self._queue) > 200:
                del self._queue[:-200]
            return
        if not self.delay:
            self._render(event)
            return

        kind = event.get("kind")
        if kind in ("delete", "purge"):
            # Модератор прибрав повідомлення, яке ще навіть не показане — тоді
            # його треба не показувати зовсім, а не показати й одразу зняти.
            self._drop_pending(event)
            self._render(event)
            return
        self._delayed.append((time.monotonic() + self.delay, event))
        if not self._timer.isActive():
            self._timer.start()

    def _drop_pending(self, event: dict):
        kind, key = event.get("kind"), ""
        if kind == "delete":
            key = event.get("id", "")
            self._delayed = [(t, e) for t, e in self._delayed if e.get("id") != key or not key]
        else:
            key = event.get("nick", "")
            self._delayed = [(t, e) for t, e in self._delayed if e.get("nick") != key or not key]

    def _flush(self):
        """Випускає те, чий час настав, — по одному рядку за такт."""
        if not self._delayed:
            self._timer.stop()
            return
        due, event = self._delayed[0]
        if time.monotonic() < due:
            return
        self._delayed.pop(0)
        self._render(event)

    def _render(self, event: dict):
        kind = event.get("kind")
        if kind == "delete":
            js = "window.fts&&fts.del(%s)" % json.dumps(event.get("id", ""))
        elif kind == "purge":
            js = "window.fts&&fts.purge(%s)" % json.dumps(event.get("nick", ""))
        else:
            js = "window.fts&&fts.add(%s)" % json.dumps(event, ensure_ascii=False)
        self.view.page().runJavaScript(js)
