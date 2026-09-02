"""Вікно «Що потрібно, щоб чат у грі запрацював».

Чесне й спокійне пояснення для інжектора: що саме робить Hominka, чому Windows
може перепитати, що (і головне — чого НЕ) вимикати, і де так робити не можна.
Виносимо це в окреме вікно, щоб у панелі лишалося коротке попередження, а не
стіна тексту, і щоб людина ухвалювала рішення свідомо, а не наосліп.
"""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QDialog, QFrame, QLabel, QPushButton, QScrollArea, QVBoxLayout, QWidget,
)


_HELP_HTML = """
<h2 style="margin:0 0 6px 0;">Справжній чат у грі — як це працює</h2>
<p>Hominka малює твій чат — з аватарками, значками й емоутами — <b>усередині
кадру гри</b>, навіть у виключному повноекранному режимі. Тим самим шляхом, що й
оверлеї OBS, Discord чи Steam: у процес гри вкладається маленька бібліотека
(<code>overlay.dll</code>), яка домальовує чат перед показом кадру. Уся важка
робота — чат, шрифти, емоути — лишається в Hominka; якщо чат «моргне», гра цього
навіть не помітить.</p>

<h3 style="margin:14px 0 4px 0;">Чому Windows може перепитати</h3>
<p>Наша бібліотека поки що <b>не має цифрового підпису</b> (сертифікат — окремі
гроші й окреме рішення). Тому при першому запуску можливе одне з двох:</p>
<ul style="margin:4px 0 0 0; padding-left:18px;">
  <li><b>SmartScreen</b> («Windows захистила ваш ПК»): натисни
      <b>«Докладніше» → «Виконати в будь-якому разі»</b>.</li>
  <li><b>Microsoft Defender</b> може прибрати файл «про всяк випадок»: тоді
      відкрий <b>Захист від вірусів → Журнал захисту</b>, знайди Hominka і обери
      <b>«Дозволити»</b>. Можна заздалегідь додати <b>папку Hominka</b> у
      «Винятки».</li>
</ul>

<div style="background:rgba(217,119,6,0.14); border:1px solid rgba(217,119,6,0.45);
     border-radius:6px; padding:8px 10px; margin:12px 0;">
<b>Не вимикай антивірус повністю.</b> Це небезпечно і непотрібно. Достатньо
<b>дозволити саме Hominka</b> (або її папку) — рівно один виняток, а не «зняти
захист з усього ПК».
</div>

<h3 style="margin:6px 0 4px 0; color:#fca5a5;">Де так робити НЕ можна</h3>
<p>В <b>онлайн-іграх зі захистом від читів</b> вкладати сторонню бібліотеку
<b>НЕ можна — це загрожує баном акаунта</b>. Сюди належать, зокрема, Valorant
(Vanguard), CS2 та FACEIT, Rust, Apex Legends (EAC), Escape from Tarkov та інші
змагальні ігри. Hominka навмисно <b>відмовляється</b> інжектити у відомі такі
ігри й коли бачить активний античит.</p>
<p>Хочеш чат в онлайн-грі — <b>переведи гру в безрамковий режим</b> (кнопка
«Зробити гру безрамковою» вище) і користуйся звичайним оверлеєм поверх неї:
жодного інжекту, жодного ризику.</p>

<h3 style="margin:14px 0 4px 0;">Що ми зробили, щоб було безпечніше</h3>
<ul style="margin:4px 0 0 0; padding-left:18px;">
  <li>Інжектор вантажить <b>лише власну</b> бібліотеку, звірену за міткою —
      його не можна нацькувати на чужий (наприклад, чит-) файл.</li>
  <li>Перед вкладенням перевіряються відомі античити — і в такому разі
      Hominka просто відмовляє.</li>
  <li>Малювання чату не чіпає логіку гри: ми лише домальовуємо картинку поверх
      готового кадру.</li>
</ul>

<p style="margin-top:12px; color:#9aa0a6;">Коротко: для одиночних ігор —
вмикай і дозволь Hominka, якщо Windows перепитає. Для онлайн-ігор — безрамковий
режим.</p>
"""


class InjectorHelpDialog(QDialog):
    """Модальне вікно з поясненням про інжект, SmartScreen і межі застосування."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Чат у грі — що потрібно")
        self.setWindowFlags(self.windowFlags() | Qt.WindowStaysOnTopHint)
        self.setMinimumSize(460, 520)
        self.setStyleSheet(
            "QDialog { background:#16181d; }"
            "QLabel { color:#e6e8eb; font-size:13px; }"
            "QScrollArea { border:none; background:transparent; }"
            "QPushButton { background:#2a2e37; color:#e6e8eb; border:none;"
            " border-radius:6px; padding:7px 16px; }"
            "QPushButton:hover { background:#343945; }")

        lay = QVBoxLayout(self)
        lay.setContentsMargins(16, 16, 16, 14)
        lay.setSpacing(10)

        scroll = QScrollArea(self)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.NoFrame)
        body = QWidget(scroll)
        body_lay = QVBoxLayout(body)
        body_lay.setContentsMargins(0, 0, 8, 0)
        text = QLabel(_HELP_HTML, body)
        text.setWordWrap(True)
        text.setTextFormat(Qt.RichText)
        text.setTextInteractionFlags(Qt.TextBrowserInteraction)
        text.setOpenExternalLinks(True)
        body_lay.addWidget(text)
        body_lay.addStretch(1)
        scroll.setWidget(body)
        lay.addWidget(scroll, 1)

        close = QPushButton("Зрозуміло", self)
        close.clicked.connect(self.accept)
        lay.addWidget(close, 0, Qt.AlignRight)
