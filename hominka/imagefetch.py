"""Качає картинки емоутів і значків — щоб віддати їх нативному рендеру байтами.

Навіщо це взагалі зʼявилося. Доки чат малював браузер, картинки він качав сам:
у розмітці стояв <img src="https://cdn.7tv.app/…">, і решта була не наша
турбота. Нативний рендер у мережу не ходить принципово — сокети, TLS і повтори
лишаються в Python, де вони вже є й перевірені. Тож той бік просить «дай
картинку за цією адресою», а цей качає й надсилає байти.

Черга і потоки. Качаємо у кількох робочих потоках, бо емоут — це десятки
кілобайт з чужого CDN, і на поганому звʼязку один такий запит стоїть секунду.
Головний потік Qt при цьому не чекає ніколи: результат приходить зворотним
викликом, а рядок у чаті доти показує код емоута («Kappa») замість картинки —
рівно як чат виглядав, доки емоутів не було взагалі.

Одна адреса качається РІВНО ОДИН РАЗ за запуск, навіть якщо її попросили сто
разів поспіль: у жвавому чаті той самий емоут іде в кожному другому рядку.
"""

import threading
import urllib.error
import urllib.request

# Скільки качаємо одночасно. Більше не треба: емоути дрібні, а десяток потоків
# на чужий CDN виглядає як напад і легко ловить обмеження швидкості.
WORKERS = 3

# Стеля на одну картинку. Емоут 7TV у 2x — це одиниці-десятки кілобайт;
# мегабайт означає, що нам віддали не те, і тягнути це в память немає сенсу.
MAX_BYTES = 4 * 1024 * 1024

TIMEOUT = 10

# Той самий Chrome-подібний заголовок, що й у решті програми: деякі CDN
# віддають 403 на «python-urllib».
_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
       "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")


class ImageFetcher:
    """Качає адреси й віддає байти зворотним викликом.

    on_ready(url, data) викликається З РОБОЧОГО ПОТОКУ. Той, хто підписався,
    сам вирішує, як передати це далі; нативному рендеру писати в канал з
    чужого потоку можна — запис у канал і так робить окремий потік.
    """

    def __init__(self, on_ready):
        self._on_ready = on_ready
        self._lock = threading.Lock()
        self._seen = set()          # адреси, які вже просили (успішно чи ні)
        self._queue = []
        self._cv = threading.Condition(self._lock)
        self._stop = False
        self._threads = []

    def start(self):
        if self._threads:
            return
        self._stop = False
        for _ in range(WORKERS):
            t = threading.Thread(target=self._work, daemon=True)
            t.start()
            self._threads.append(t)

    def stop(self):
        with self._cv:
            self._stop = True
            self._cv.notify_all()
        self._threads = []

    def want(self, url: str):
        """Попросити картинку. Повторні прохання про ту саму адресу — безкоштовні."""
        if not url or url.startswith("data:"):
            # «data:» самодостатня: нативний бік розбере її сам, качати нічого.
            return
        with self._cv:
            if url in self._seen:
                return
            self._seen.add(url)
            self._queue.append(url)
            self._cv.notify()

    def forget(self, url: str):
        """Забути, що адресу вже просили — щоб спробувати ще раз."""
        with self._cv:
            self._seen.discard(url)

    def _work(self):
        while True:
            with self._cv:
                while not self._queue and not self._stop:
                    self._cv.wait()
                if self._stop:
                    return
                url = self._queue.pop(0)
            data = self._fetch(url)
            if data:
                try:
                    self._on_ready(url, data)
                except Exception:
                    # Зворотний виклик не має права зупинити качалку: наступні
                    # емоути потрібні незалежно від того, що сталося з цим.
                    pass
            else:
                # Не вийшло — знімаємо позначку, щоб наступна поява емоута дала
                # ще одну спробу. Нескінченного циклу з цього не виходить:
                # просять лише тоді, коли емоут трапився в новому повідомленні.
                self.forget(url)

    @staticmethod
    def _fetch(url: str):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": _UA})
            with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
                data = r.read(MAX_BYTES + 1)
            if not data or len(data) > MAX_BYTES:
                return None
            return data
        except (urllib.error.URLError, OSError, ValueError):
            return None
