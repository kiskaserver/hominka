"""Оновлення: перевірка, завантаження, встановлення.

Частина вікна (домішується в Overlay). Тут же — увесь зв'язок зі смужкою
оновлення: вікно про неї знає лише те, що вона є.
"""

import os
import sys

from PySide6.QtCore import QTimer

from . import update as updater
from .notes import update_status_html
from .paths import BASE_DIR
from .version import APP_VERSION


class UpdatingMixin:
    """Хід оновлення від перевірки до перезапуску. Частина Overlay."""

    # --- оновлення ---
    def set_channel(self, channel: str):
        """Зміна каналу оновлень. Перевіряємо одразу: людина щойно попросила
        іншу гілку — вона й чекає результату, а не наступної перевірки за 6 годин."""
        if not channel or channel == self.channel:
            return
        self.channel = channel
        self.save_config()
        self.check_updates(manual=True)

    def check_updates(self, manual: bool = False):
        if getattr(self, "updater", None) is None:
            return
        if not getattr(sys, "frozen", False):
            # Запущено з коду — підміняти теку нічим і нема чого.
            if manual:
                self.panel.set_status("Запущено з коду — оновлення не застосовуються.")
            return
        if self.updater.busy:
            return
        self._manual_check = manual
        self.panel.set_status("Перевіряю оновлення…")
        self.updater.check(self.channel, self.installed_channel)

    def _on_checked(self, rel, err: str):
        if err:
            self.panel.set_status("Не вдалося перевірити: " + err)
            return
        if rel is None:
            self.pending = None
            self.panel.set_status("Версія %s — актуальна (%s)."
                                  % (APP_VERSION, updater.channel_label(self.channel)))
            return
        self.pending = rel
        self.panel.set_status(update_status_html(rel))
        self.banner.show_release(rel)
        # Панель налаштувань — окреме вікно поверх; поки вона відкрита, смужку
        # з оновленням видно погано. Ховаємо: рішення тепер приймають у ній.
        if self.panel.isVisible():
            self.panel.hide()
        # Обов'язкове оновлення — це виправлення, без якого програма працює
        # неправильно. Не ставимо мовчки, але й не даємо про нього забути.
        if rel.mandatory and self.auto_update:
            self.start_update()

    def start_update(self):
        if self.pending is None:
            self.check_updates(manual=True)
            return
        # Далі програма перезапуститься — тримати відкриту панель ні до чого.
        self.panel.hide()
        self.banner.show_progress(0, self.pending.size)
        self.updater.download(self.pending)

    def _on_progress(self, done: int, total: int):
        self.banner.show_progress(done, total)

    def on_update_button(self):
        """Одна кнопка на два кроки: спершу завантажити, потім встановити."""
        if self.downloaded:
            self.install_update()
        else:
            self.start_update()

    def _on_downloaded(self, path: str):
        self.downloaded = path
        self.banner.show_ready(self.pending) if self.pending else None
        self.panel.set_status("Завантажено %s — натисніть «Встановити»."
                              % (self.pending.version if self.pending else ""))

    def install_update(self):
        """Ставить завантажене й виходить: підмінник чекає саме виходу.

        Спершу показуємо, що відбувається, і лише потім закриваємось: інакше
        натискання «Встановити» виглядає як вилітання програми.
        """
        if not self.downloaded:
            return
        self.banner.show_installing(self.pending.version if self.pending else "")
        QTimer.singleShot(1200, self._do_install)

    def _do_install(self):
        if not self.downloaded:
            return
        try:
            app_dir = os.path.dirname(sys.executable) if getattr(sys, "frozen", False) else BASE_DIR
            # Канал запам'ятовуємо ДО перезапуску: після нього це вже інша збірка,
            # і без запису вона не знала б, з якої гілки прийшла.
            self.installed_channel = self.channel
            # Позначка «ми щойно оновлювались»: після перезапуску за нею
            # програма скаже, що саме сталося, — інакше вона просто зникає й
            # з'являється, і зрозуміти це неможливо.
            self.updated_to = self.pending.version if self.pending else ""
            self.updated_notes = self.pending.notes if self.pending else ""
            self._write_config()
            updater.install(self.downloaded, app_dir)
        except Exception as e:
            self.banner.show_error(str(e))
            return
        self.downloaded = ""
        self.close()

    def _on_update_failed(self, msg: str):
        self.banner.show_error(msg)
        self.panel.set_status("Оновлення не вдалося: " + msg)
