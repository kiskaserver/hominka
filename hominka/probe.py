"""Пошук власної трансляції на YouTube — прихованою сторінкою.

Програма для того й є, щоб стрімер читав СВІЙ чат, тому вставляти посилання
перед кожним ефіром безглуздо: досить один раз назвати канал.
"""

import json

from PySide6.QtCore import QObject, QTimer, QUrl, Signal
from PySide6.QtWebEngineCore import QWebEnginePage, QWebEngineProfile

from .urls import channel_id_from, channel_page_url

# JS: id і назва каналу зі сторінки самого каналу (для поля «Мій канал»).
JS_CHANNEL_PAGE = r"""
(function () {
  var id = '', title = '';
  try { id = window.ytInitialData.metadata.channelMetadataRenderer.externalId || ''; } catch (e) {}
  try { title = window.ytInitialData.metadata.channelMetadataRenderer.title || ''; } catch (e) {}
  if (!id) {
    var l = document.querySelector('link[rel="canonical"]');
    if (l) { var m = l.href.match(/UC[0-9A-Za-z_\-]{22}/); if (m) id = m[0]; }
  }
  return JSON.stringify({id: id, title: title});
})();
"""

# JS: дістати id живої трансляції зі сторінки каналу /live.
JS_LIVE_VIDEO = r"""
(function () {
  var d = window.ytInitialData;
  if (!d) return '';
  var v = '';
  try { v = d.currentVideoEndpoint.watchEndpoint.videoId || ''; } catch (e) { v = ''; }
  if (!v) return '';
  // Сторінка перегляду буває і в завершеного ефіру, і в анонса — беремо тільки
  // те, що зараз в ефірі.
  var live = JSON.stringify(d).indexOf('"isLive":true') >= 0;
  // Назву каналу YouTube кладе по-різному: на сторінці каналу — в метаданих,
  // на сторінці перегляду (куди веде /live під час ефіру) — біля відео.
  var title = '';
  try { title = d.metadata.channelMetadataRenderer.title || ''; } catch (e) {}
  if (!title) {
    try { title = window.ytInitialPlayerResponse.videoDetails.author || ''; } catch (e) {}
  }
  if (!title) {
    var el = document.querySelector('ytd-video-owner-renderer #channel-name a, #owner #channel-name a');
    if (el) title = (el.textContent || '').trim();
  }
  var logged = null;
  try { logged = !!window.ytcfg.get('LOGGED_IN'); } catch (e) { logged = null; }
  return JSON.stringify({video: live ? v : '', title: title, logged: logged});
})();
"""


class LiveProbe(QObject):
    """Тихо шукає активну трансляцію заданого каналу.

    Працює на прихованій сторінці в тому ж профілі, тож вікно чату нічого не
    перемальовує.

    Дві сторінки, обидві відкриті всім:
      • сторінка каналу — щоб із «@нік» дістати UC-id (робиться один раз);
      • /channel/<id>/live — щоб дізнатися, чи йде ефір і який у нього id відео.

    На сторінки входу Google не заходимо ніколи: прихована сторінка, що туди
    потрапляє, викликає системне вікно passkey — воно вискакує посеред гри, і
    зрозуміти, звідки воно, неможливо.
    """

    # {"video", "channelId", "title"}
    result = Signal(dict)

    def __init__(self, profile: QWebEngineProfile, parent=None):
        super().__init__(parent)
        self.page = QWebEnginePage(profile, self)
        self.page.loadFinished.connect(self._on_loaded)
        # Підтверджувати щось на прихованій сторінці нікому: гасимо запити
        # passkey, якщо вони раптом виникнуть.
        if hasattr(self.page, "webAuthUxRequested"):
            self.page.webAuthUxRequested.connect(self._deny_webauth)

        self.channel_id = ""
        self.channel_title = ""
        self._step = ""          # resolve | live
        self._guard = QTimer(self)
        self._guard.setSingleShot(True)
        self._guard.setInterval(30000)   # сторінка не відповіла — не висимо вічно
        self._guard.timeout.connect(self._give_up)

    @property
    def busy(self) -> bool:
        return bool(self._step)

    @staticmethod
    def _deny_webauth(request):
        try:
            request.cancel()
        except Exception:
            pass

    def start(self, manual_channel: str = "", channel_id: str = ""):
        """manual_channel — те, що вписали в «Мій канал»; channel_id — вже
        відомий UC-id (щоб не резолвити «@нік» щоразу)."""
        if self._step:
            return
        manual = (manual_channel or "").strip()
        self.channel_id = channel_id_from(manual) or channel_id or self.channel_id
        if self.channel_id:
            self._go("live", "https://www.youtube.com/channel/%s/live?hl=en" % self.channel_id)
        elif manual:
            self._go("resolve", channel_page_url(manual))
        else:
            self._finish()       # каналу не задано — шукати нічого

    def _go(self, step: str, url: str):
        self._step = step
        self.page.setUrl(QUrl(url))
        self._guard.start()

    def _finish(self, video: str = ""):
        self._guard.stop()
        self._step = ""
        self.result.emit({
            "video": video,
            "channelId": self.channel_id,
            "title": self.channel_title,
        })

    def _give_up(self):
        self._step = ""
        self._finish()

    def _on_loaded(self, ok: bool):
        if not self._step:
            return
        if not ok:
            self._finish()
            return
        if self._step == "resolve":
            self.page.runJavaScript(JS_CHANNEL_PAGE, self._got_resolve)
        else:
            self.page.runJavaScript(JS_LIVE_VIDEO, self._got_live)

    def _parse(self, value):
        try:
            return json.loads(value) if value else {}
        except (ValueError, TypeError):
            return {}

    def _got_resolve(self, value):
        data = self._parse(value)
        cid = data.get("id") or ""
        self.channel_title = data.get("title") or self.channel_title
        if not cid:
            self._finish()   # такого каналу немає — це видно в налаштуваннях
            return
        self.channel_id = cid
        self._go("live", "https://www.youtube.com/channel/%s/live?hl=en" % cid)

    def _got_live(self, value):
        data = self._parse(value)
        self.channel_title = data.get("title") or self.channel_title
        self._finish(data.get("video") or "")
