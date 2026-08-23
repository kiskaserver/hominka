"""Оформлення вікна і панелі — в одному місці.

Стилі Qt тут навмисно рядками, а не окремим .qss: їх треба бачити поруч із
кольорами акценту, від яких вони залежать.
"""

ACCENT_ACTIVE = "#a855f7"   # рамка у звичайному режимі (фіолетова)


ACCENT_LOCKED = "#22c55e"   # рамка у режимі клік-крізь (зелена)


BTN_CSS = """
QPushButton {
    background: rgba(255,255,255,0.06);
    color: #e4e4e7;
    border: none;
    border-radius: 6px;
    font: 12px 'Segoe UI';
}
QPushButton:hover { background: rgba(255,255,255,0.16); color: #fff; }
QPushButton:pressed { background: rgba(255,255,255,0.24); }
"""


SLIDER_CSS = """
QSlider { max-height: 16px; }
QSlider::groove:horizontal {
    height: 4px; border-radius: 2px; background: rgba(255,255,255,0.18);
}
QSlider::sub-page:horizontal { background: %s; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 12px; height: 12px; margin: -5px 0; border-radius: 6px;
    background: #ffffff;
}
"""


# Вигляд вікна налаштувань. Один рядок стилю на все вікно: інакше кожен віджет
# обростає власним setStyleSheet, і зібрати з цього цілісний вигляд неможливо.
PANEL_CSS = """
#body {
    background: #17141f;
    border: 1px solid rgba(168,85,247,0.35);
    border-radius: 14px;
}
QLabel { color: #d4d4d8; font: 12px 'Segoe UI'; }
QLabel#title { color: #fafafa; font: 600 14px 'Segoe UI'; }
QLabel#cap {
    color: #c4b5fd; font: 600 11px 'Segoe UI';
    text-transform: uppercase; letter-spacing: 1px;
}
QLabel#field { color: #a1a1aa; font: 11px 'Segoe UI'; }
QLabel#dim   { color: #8b8b93; font: 11px 'Segoe UI'; }
QLabel#value { color: #e9d5ff; font: 600 11px 'Segoe UI'; }
QLabel#error { color: #fca5a5; font: 11px 'Segoe UI'; }

#card {
    background: rgba(255,255,255,0.035);
    border: 1px solid rgba(255,255,255,0.07);
    border-radius: 10px;
}

QLineEdit {
    background: rgba(255,255,255,0.06); color: #fafafa;
    border: 1px solid rgba(255,255,255,0.12); border-radius: 7px;
    padding: 5px 8px; font: 12px 'Segoe UI';
    selection-background-color: #a855f7;
}
QLineEdit:focus { border: 1px solid #a855f7; background: rgba(255,255,255,0.09); }

QPushButton#primary {
    background: #a855f7; color: #fff; border: none;
    border-radius: 7px; font: 600 12px 'Segoe UI';
}
QPushButton#primary:hover { background: #9333ea; }
QPushButton#primary:pressed { background: #7e22ce; }

QPushButton#ghost {
    background: rgba(255,255,255,0.06); color: #e4e4e7; border: none;
    border-radius: 7px; font: 12px 'Segoe UI';
}
QPushButton#ghost:hover { background: rgba(255,255,255,0.14); color: #fff; }
QPushButton#ghost:pressed { background: rgba(255,255,255,0.2); }

QComboBox {
    background: rgba(255,255,255,0.06); color: #fafafa;
    border: 1px solid rgba(255,255,255,0.12); border-radius: 7px;
    padding: 4px 8px; font: 12px 'Segoe UI';
    min-height: 18px;
}
QComboBox:hover { border: 1px solid #a855f7; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView {
    background: #17141f; color: #fafafa; selection-background-color: #a855f7;
    border: 1px solid rgba(255,255,255,0.14); outline: none; padding: 2px;
}

QCheckBox { color: #d4d4d8; font: 11px 'Segoe UI'; spacing: 7px; }
QCheckBox::indicator {
    width: 14px; height: 14px; border-radius: 4px;
    border: 1px solid rgba(255,255,255,0.28); background: rgba(255,255,255,0.06);
}
QCheckBox::indicator:hover { border: 1px solid rgba(168,85,247,0.7); }
/* Галочку в стилях не намалюєш без картинки, тому «увімкнено» — заповнений
   квадрат із темною серединою: помітно і не потребує зайвих файлів. */
QCheckBox::indicator:checked {
    background: #a855f7; border: 4px solid #17141f;
    width: 8px; height: 8px; border-radius: 6px;
}
"""


PROGRESS_CSS = """
QProgressBar {
    background: rgba(255,255,255,0.10); border: none; border-radius: 4px;
    height: 8px; text-align: center; color: transparent;
}
QProgressBar::chunk { background: #a855f7; border-radius: 4px; }
"""


INPUT_CSS = """
QLineEdit {
    background: rgba(255,255,255,0.07); color: #fafafa;
    border: 1px solid rgba(255,255,255,0.14); border-radius: 6px;
    padding: 4px 7px; font: 12px 'Segoe UI';
    selection-background-color: #a855f7;
}
QLineEdit:focus { border: 1px solid #a855f7; }
"""


# Сторінка на місці чату, поки джерело не задано.
NO_SOURCE_HTML = """
<html><head><meta charset="utf-8"><style>
  /* overflow:hidden — вікно чату без смуги прокрутки. Вона тут не потрібна
     нікому: підказка коротка, а смуга збоку виглядає як зламана сторінка. */
  html,body{margin:0;height:100%;overflow:hidden;background:transparent;
            font:14px/1.5 "Segoe UI",sans-serif;color:#e7e2df}
  div{box-sizing:border-box;height:100%;display:flex;flex-direction:column;gap:8px;
      align-items:center;justify-content:center;text-align:center;padding:16px}
  b{color:#c9a4ff;font-size:14px}
  span{color:#9a9490;font-size:12px}
</style></head><body><div>
  <b>Чат ще не вибрано</b>
  <span>Натисни ⚙ і впиши свій канал —<br>YouTube, Twitch або Kick.</span>
</div></body></html>
"""
