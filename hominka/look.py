"""Вигляд вікна: рамка, прозорість підкладки, кегль, клік-крізь.

Частина вікна (домішується в Overlay). Винесено окремо, бо це єдина група, яку
людина крутить постійно — повзунками в ⚙, — і вона не має нічого спільного ні з
джерелами чату, ні з оновленнями.
"""

from .winapi import set_click_through


class LookMixin:
    """Рамка, кегль, прозорість і клік-крізь. Частина Overlay."""

    def _apply_border(self, accent: str):
        self.accent = accent
        self.frame.setStyleSheet(
            "#frame {"
            f" background: rgba(12,12,15,{self.bg_alpha:.2f});"
            f" border: 2px solid {accent};"
            " border-radius: 11px;"
            " }"
        )
        if hasattr(self, "grip"):
            self.grip.set_accent(accent)

    # --- розмір тексту ---
    def zoom_in(self):
        self.set_zoom(self.zoom + 0.1)

    def zoom_out(self):
        self.set_zoom(self.zoom - 0.1)

    def set_zoom(self, z: float):
        self.zoom = max(0.5, min(3.0, round(z, 2)))
        self.view.setZoomFactor(self.zoom)
        self.panel.sync_zoom()
        self.save_config()

    # --- тло ---
    def set_bg_alpha(self, v: int):
        self.bg_alpha = max(0.0, min(1.0, v / 100))
        self._apply_chrome()
        # Те саме тло — у чат поверх гри (dcomp/інжект): його малює продюсер у
        # своєму кадрі, тож підкладку треба оновити й там.
        if getattr(self, "game_overlay", None) is not None:
            self.game_overlay.set_bg_alpha(self.bg_alpha)
        self.save_config()

    # --- рамка / чисті повідомлення ---
    def _apply_chrome(self):
        """Показ рамки, заголовка й куточка залежно від режиму «без рамки» та
        замка (клік-крізь).

        Задум (узгоджено): без рамки + замок УВІМКНЕНО (клік провалюється крізь,
        стрімер дивиться ефір) → лише повідомлення, жодного хрому. Без рамки +
        РОЗБЛОКОВАНО → повертаємо заголовок, тонку рамку й куточок, щоб було за що
        взятися: посунути, відкрити налаштування, замкнути. Зі звичайною рамкою
        (frameless=False) хром показуємо завжди. Клік-крізь і так означає «я це
        вікно не чіпаю», тож ховати хром саме тоді — природно."""
        chrome = (not getattr(self, "frameless", False)) or (not self.click_through)
        self.bar.setVisible(chrome)
        if hasattr(self, "grip"):
            self.grip.setVisible(chrome)
        if chrome:
            self._apply_border(self.accent)            # фіолетова рамка + підкладка
        else:
            # «Без рамки»: прибираємо БОРДЮР і заголовок, але ПІДКЛАДКУ лишаємо —
            # нею керує повзунок «Тло під чатом» (bg_alpha). Хоче зовсім без тла —
            # виставить повзунок на 0. Тобто «рамка» ≠ «фон».
            self.frame.setStyleSheet(
                "#frame {"
                f" background: rgba(12,12,15,{self.bg_alpha:.2f});"
                " border: none;"
                " border-radius: 11px;"
                " }")

    def set_frameless(self, on: bool):
        """Увімкнути/вимкнути режим «без рамки» (чисті повідомлення)."""
        self.frameless = bool(on)
        self._apply_chrome()
        self.save_config()

    def toggle_click_through(self):
        self.click_through = not self.click_through
        set_click_through(self, self.click_through)
        # Статус блокування показуємо кружечком біля назви: зелений — миша
        # провалюється крізь вікно (замок увімкнено), фіолетовий — вікно ловить
        # мишу. Рамку в зелений більше НЕ фарбуємо (заважала) — усе фіолетове.
        self.bar.set_locked(self.click_through)
        # У режимі «без рамки» замок ще й ховає/повертає хром (див. _apply_chrome).
        self._apply_chrome()
