"""Смужка «є оновлення» під панеллю вікна."""

from typing import TYPE_CHECKING

from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QProgressBar, QPushButton, QVBoxLayout

from .. import updater
from ..styles import BTN_CSS, PROGRESS_CSS

if TYPE_CHECKING:                      # тільки для підказок типів
    from ..overlay import Overlay


class UpdateBanner(QFrame):
    """Смужка «є оновлення» під панеллю вікна.

    Не діалог і не спливаюче вікно: програма висить поверх гри, і модальне
    вікно посеред бою — гірше за будь-яке оновлення. Смужку видно, коли є що
    ставити, і вона зникає, щойно користувач вирішив.
    """

    def __init__(self, win: "Overlay"):
        super().__init__(win)
        self.win = win
        self.setObjectName("upd")
        self.setStyleSheet(
            "#upd { background: rgba(168,85,247,0.22);"
            " border-bottom: 1px solid rgba(168,85,247,0.45); }"
            "QLabel { color: #f5e9ff; font: 11px 'Segoe UI'; }"
        )
        lay = QHBoxLayout(self)
        lay.setContentsMargins(10, 5, 6, 5)
        lay.setSpacing(6)

        # Дві сходинки замість одного рядка: зверху — що саме прийшло, під ним
        # дрібнішим — що змінилося. Раніше опис змін був лише в підказці при
        # наведенні та в ⚙, тобто там, куди людина не дивиться, і оновлення
        # виглядало як пропозиція «постав щось».
        col = QVBoxLayout()
        col.setContentsMargins(0, 0, 0, 0)
        col.setSpacing(1)
        self.text = QLabel("", self)
        self.text.setWordWrap(True)
        self.text.setMinimumWidth(0)     # інакше QLabel вимагає ширини на весь рядок
        col.addWidget(self.text)
        self.note = QLabel("", self)
        self.note.setWordWrap(True)
        self.note.setMinimumWidth(0)
        self.note.setStyleSheet("color:#d6c7e8; font:10px 'Segoe UI';")
        self.note.hide()
        col.addWidget(self.note)
        lay.addLayout(col, 1)

        self.bar = QProgressBar(self)
        self.bar.setStyleSheet(PROGRESS_CSS)
        self.bar.setFixedWidth(90)
        self.bar.hide()
        lay.addWidget(self.bar)

        self.go = QPushButton("Оновити", self)
        self.go.setFixedHeight(24)
        self.go.setStyleSheet(BTN_CSS + "QPushButton{background:#a855f7;color:#fff;font-weight:600;padding:0 8px;}"
                              "QPushButton:hover{background:#9333ea;}")
        self.go.clicked.connect(win.on_update_button)
        lay.addWidget(self.go)

        later = QPushButton("✕", self)
        later.setToolTip("Пізніше")
        later.setFixedSize(22, 22)
        later.setStyleSheet(BTN_CSS)
        later.clicked.connect(self.hide)
        lay.addWidget(later)

        self.hide()

    def show_release(self, rel):
        """Версія, вид оновлення і — головне — що в ньому змінилося."""
        self.text.setText("Є оновлення %s · %s" % (rel.version, updater.kind_label(rel.kind)))
        note = " ".join((rel.notes or "").split())
        self._set_note(note)
        self.text.setToolTip(note)
        self.setToolTip(note)
        self.bar.hide()
        self.go.show()
        self.go.setEnabled(True)
        self.go.setText("Оновити")
        self.show()

    def _set_note(self, text: str):
        self.note.setText(text)
        self.note.setVisible(bool(text))

    def show_installing(self, version: str):
        """Пояснює, чому вікно зараз зникне.

        Підмінник чекає саме нашого виходу, тож програма мусить закритися — але
        без цього рядка вона просто пропадала з екрана, і людина лишалася з
        думкою, що оновлення її зламало.
        """
        self.text.setText("Ставлю оновлення %s…" % version)
        self._set_note("Вікно зараз закриється і за кілька секунд відкриється саме.")
        self.bar.hide()
        self.go.setEnabled(False)
        self.go.setText("Ставлю…")
        self.show()

    def show_updated(self, version: str, notes: str = ""):
        """Після перезапуску: оновлення справді сталося, ось воно."""
        self.text.setText("Оновлено до %s" % version)
        self._set_note(" ".join((notes or "").split()))
        self.bar.hide()
        self.go.hide()
        self.show()

    def show_ready(self, rel):
        """Завантажено — тепер рішення за людиною.

        Раніше програма ставила оновлення одразу після завантаження й сама
        перезапускалася: посеред стріму це щонайменше неввічливо.
        """
        self.text.setText("Оновлення %s завантажено" % rel.version)
        self._set_note("Натисніть «Встановити» — програма перезапуститься.")
        self.bar.hide()
        self.go.setEnabled(True)
        self.go.setText("Встановити")
        self.show()

    def show_progress(self, done: int, total: int):
        self.bar.show()
        self.go.setEnabled(False)
        self.go.setText("Качаю…")
        if total > 0:
            self.bar.setRange(0, 100)
            self.bar.setValue(int(done * 100 / total))
        else:
            self.bar.setRange(0, 0)  # невідомий розмір — «біжуча» смужка

    def show_error(self, msg: str):
        self.text.setText("Оновлення не вдалося")
        self._set_note(msg)
        self.bar.hide()
        self.go.show()
        self.go.setEnabled(True)
        self.go.setText("Ще раз")
        self.show()
