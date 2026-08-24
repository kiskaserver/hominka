"""Звідки береться чат: площадки, спільна стрічка, власний ефір.

Частина вікна (домішується в Overlay), винесена окремо: додати площадку чи
змінити правила вибору джерела — найчастіша правка в програмі, і робити її в
файлі, де поруч лежить робота з рамкою вікна, незручно.
"""

from PySide6.QtCore import QUrl

from . import chat_kick, chat_twitch, chatsources as cs
from . import youtube as chat_youtube
from .styles import NO_SOURCE_HTML
from .urls import channel_id_from, is_youtube, resolve_chat_url


class SourcesMixin:
    """Вибір джерела чату і читачі площадок. Частина Overlay."""

    # --- вхід у YouTube ---
    def set_my_channel(self, text: str):
        """Канал, заданий руками. Скидаємо знайдений id: вписали інший канал —
        шукати треба заново."""
        text = (text or "").strip()
        if text == self.my_channel:
            return
        self.my_channel = text
        self.yt_channel_id = channel_id_from(text)
        self.probe.channel_id = self.yt_channel_id
        self.probe.channel_title = ""
        self.auto_video = ""
        self.save_config()
        self.panel.set_source_status(self)
        self.probe_live()

    def set_site_url(self, text: str):
        """Посилання на чат сайту з ⚙. Порожнє — теж відповідь: чат сайту не
        показуємо взагалі, залишаються площадки."""
        text = (text or "").strip()
        if text == self.site_url:
            return
        self.site_url = text
        self.save_config()
        self.panel.set_source_status(self)
        self.refresh_source()

    def set_chat_delay(self, seconds: int):
        self.chat_delay = max(0, int(seconds))
        if self.feed is not None:
            self.feed.set_delay(self.chat_delay)
        self.save_config()

    def set_extra_channels(self, twitch: str, kick: str):
        """Канали Twitch і Kick із налаштувань. Приймаємо і посилання, і нік."""
        tw_platform, tw = cs.parse_source(twitch)
        kk_platform, kk = cs.parse_source(kick)
        # Голий нік площадка не вгадає — тоді беремо як є, з відповідного поля.
        if not tw and twitch.strip():
            tw = twitch.strip().lower().lstrip("@")
        if not kk and kick.strip():
            kk = kick.strip().lower().lstrip("@")
        if tw_platform and tw_platform != cs.TWITCH:
            tw = ""
        if kk_platform and kk_platform != cs.KICK:
            kk = ""
        if tw == self.twitch_channel and kk == self.kick_channel:
            return
        self.twitch_channel, self.kick_channel = tw, kk
        self.save_config()
        self.panel.set_source_status(self)
        self.refresh_source()

    # --- пошук власної трансляції ---
    def probe_live(self):
        """Питає YouTube, чи йде зараз ефір на нашому каналі."""
        if self.probe.busy:
            return
        self.probe.start(self.my_channel, self.yt_channel_id)

    def _on_probe(self, info: dict):
        self.yt_channel_id = info.get("channelId") or ""
        self.yt_channel_title = info.get("title") or ""
        video = info.get("video") or ""
        changed = video != self.auto_video
        self.auto_video = video
        self.save_config()
        self.panel.set_source_status(self)
        # У режимі спільної стрічки трансляцію шукає сам читач YouTube, і
        # перезбирати стрічку через нього не можна: це стерло б уже показані
        # повідомлення Twitch і Kick.
        if changed and self.mode != "feed":
            # Перезавантажуємо вікно, лише коли джерело справді змінилося:
            # смикати чат кожні кілька хвилин — гірше, ніж будь-яка автоматика.
            self.refresh_source()
        self._probe_timer.setInterval(self._probe_interval())

    def _probe_interval(self) -> int:
        """Ефір знайдено — перевіряємо рідко (раптом почався інший).
        Не знайдено — частіше, щоб чат з'явився невдовзі після старту стріму."""
        return 10 * 60 * 1000 if self.auto_video else 3 * 60 * 1000

    # --- джерело чату ---
    def active_sources(self) -> list:
        """Площадки, з яких зараз читаємо (для підпису в налаштуваннях)."""
        out = []
        if self.twitch_channel:
            out.append("Twitch")
        if self.kick_channel:
            out.append("Kick")
        if self.my_channel.strip() or self.yt_channel_id:
            out.append("YouTube")
        return out or (["чат сайту"] if self.site_url.strip() else ["нічого"])

    def refresh_source(self):
        """Переобчислює джерело чату і, якщо воно змінилося, відкриває його.

        Джерела — тільки ніки площадок із налаштувань. Спільна стрічка
        вмикається, щойно задано Twitch або Kick: двох чатів однією сторінкою
        YouTube не покажеш, та й іконка площадки потрібна саме тоді, коли
        джерело не одне.
        """
        if self.twitch_channel or self.kick_channel:
            self._start_feed()
            return
        self._stop_readers()
        url = resolve_chat_url(self._cli_url or "", self.auto_video, self.site_url)
        self.is_yt = is_youtube(url)
        self.bar.title.setText(self._title_for())
        if url != self.url or self.mode == "feed":
            self.mode = "web"
            self.url = url
            self._show_url()
        # Оверлей у грі має показувати те саме джерело.
        if getattr(self, "game_overlay", None) is not None:
            self._sync_game_source()

    def _show_url(self):
        """Відкриває поточне посилання або пояснює, чого бракує.

        Порожня адреса — не помилка програми, а незаповнене налаштування:
        показати білу сторінку означало б залишити людину гадати, що зламалося.
        """
        if self.url:
            self.view.load(QUrl(self.url))
        else:
            self.view.setHtml(NO_SOURCE_HTML)

    def _start_feed(self):
        """Вмикає спільну стрічку і піднімає читачів для заданих площадок."""
        self._stop_readers()
        self.mode = "feed"
        self.is_yt = False
        self.url = ""
        self.bar.title.setText(self._title_for())
        self.feed.set_delay(self.chat_delay)
        self.feed.load()

        if self.twitch_channel:
            self._add_reader(chat_twitch.TwitchChat(self.twitch_channel, self))
        if self.kick_channel:
            self._add_reader(chat_kick.KickChat(self.kick_channel, self))
        # YouTube у стрічку беремо лише за заданим каналом: без нього шукати
        # нема чого, а посилання на чужий чат іде звичайним шляхом.
        yt_channel = self.my_channel.strip() or self.yt_channel_id
        if yt_channel:
            self._add_reader(chat_youtube.YouTubeChat(yt_channel, self))
        # Назву оновлюємо ПІСЛЯ читачів: до цього active_sources() ще не знає,
        # звідки саме береться чат.
        self.bar.title.setText(self._title_for())
        # Оверлей у грі — на ту саму стрічку.
        if getattr(self, "game_overlay", None) is not None:
            self._sync_game_source()

    def _add_reader(self, reader):
        reader.event.connect(self._on_chat_event)
        reader.status.connect(self._on_reader_status)
        reader.start()
        self.readers.append(reader)

    def _stop_readers(self):
        for r in self.readers:
            r.stop()
        self.readers = []

    def _on_chat_event(self, event: dict):
        # Копія у гру, якщо ввімкнено (див. gameoverlay.py). Незалежно від режиму
        # головного вікна: у грі чат потрібен і тоді, коли на сайті показано
        # сторінку YouTube, а не спільну стрічку.
        if getattr(self, "game_on", False) and self.game_overlay is not None:
            self.game_overlay.push(event)
        if self.mode == "feed" and self.feed is not None:
            self.feed.push(event)

    def _on_reader_status(self, text: str):
        if hasattr(self, "panel"):
            self.panel.set_chat_error(text)

    def _title_for(self) -> str:
        """Назва у смужці вікна: спершу програма, потім що саме показано.

        Просто «Немає джерела» читалося як помилка невідомо чия — на екрані ж
        не написано, чиє це вікно.
        """
        if self.mode == "feed":
            sources = ", ".join(self.active_sources())
            return "Hominka — %s" % sources if sources else "Hominka"
        if self.auto_video:
            return "Hominka — мій ефір"
        if self.url:
            return "Hominka — чат"
        return "Hominka — джерело не вибрано"
