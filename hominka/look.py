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
        self._apply_border(self.accent)
        self.save_config()

    def toggle_click_through(self):
        self.click_through = not self.click_through
        set_click_through(self, self.click_through)
        # Статус блокування показуємо ЛИШЕ кружечком біля назви: зелений — миша
        # провалюється крізь вікно (замок увімкнено), фіолетовий — вікно ловить
        # мишу. Рамку (і панель, і куточок) у зелений більше НЕ фарбуємо: зелена
        # рамка надто впадала в око й заважала. Усе лишається фіолетовим.
        self.bar.set_locked(self.click_through)
