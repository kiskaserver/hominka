"""Дрібні деталі панелі: картка-рамка, підпис поля, рядок із повзунком."""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QSlider, QVBoxLayout

from ...styles import ACCENT_ACTIVE, SLIDER_CSS


class WidgetsMixin:
    """Будівельні дрібниці панелі. Частина SettingsPanel."""

    # --- будівельні дрібниці -------------------------------------------------
    def _card(self, title: str):
        card = QFrame(self)
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(11, 9, 11, 11)
        lay.setSpacing(5)
        cap = QLabel(title, card)
        cap.setObjectName("cap")
        lay.addWidget(cap)
        return card, lay

    def _label(self, text: str) -> QLabel:
        lab = QLabel(text, self)
        lab.setObjectName("field")
        return lab

    def _slider_row(self, parent_lay, lo, hi, val, cb, suffix="%"):
        row = QHBoxLayout()
        row.setSpacing(8)
        sld = QSlider(Qt.Horizontal, self)
        sld.setRange(lo, hi)
        sld.setValue(val)
        sld.setStyleSheet(SLIDER_CSS % ACCENT_ACTIVE)
        sld.valueChanged.connect(cb)
        row.addWidget(sld, 1)
        pct = QLabel(f"{val}{suffix}", self)
        pct.setObjectName("value")
        pct.setFixedWidth(40)
        pct.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        row.addWidget(pct)
        parent_lay.addLayout(row)
        return sld, pct
