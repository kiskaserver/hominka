"""
Редактор власного CSS для чату — окреме повноцінне вікно.

Навіщо. Оформлення чату в кадрі — це частина оформлення каналу, і воно у всіх
різне: комусь треба крупніше, комусь без плашок, комусь свій колір нікам з
Twitch. Робити під кожен смак галочку в налаштуваннях — безнадійно, а ось
пустити людину в CSS — рівно те, що потрібно: сторінка чату і так своя, значить
її можна стилізувати.

Чому окреме вікно, а не поле в панелі. У поле два на три сантиметри код не
пишуть. Тут: редактор із нумерацією рядків, поруч — живий перегляд на
прикладах з усіх площадок, список усіх класів з поясненнями, типовий CSS
(звідки копіювати правило, щоб перебити), перевірка синтаксису з номерами
рядків. Тобто інструмент, а не «вставте текст».

Вікно так само сховане від захоплення екрана: у Windows це робить спільний
фільтр подій (CaptureGuard у chat_overlay), який ловить показ БУДЬ-ЯКОГО вікна
програми. Тобто редактор можна відкрити просто під час ефіру.
"""

import json
import re

from PySide6.QtCore import Qt, QRect, QSize, QTimer, QUrl
from PySide6.QtGui import QColor, QFont, QPainter, QTextFormat, QTextCursor
from PySide6.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QSplitter, QPlainTextEdit,
    QTabWidget, QTreeWidget, QTreeWidgetItem, QPushButton, QLabel, QListWidget,
    QListWidgetItem, QTextEdit,
)
from PySide6.QtWebEngineWidgets import QWebEngineView

from . import chatfeed

# --- що взагалі можна стилізувати ------------------------------------------
#
# Це не «документація десь у файлі», а те, що людина бачить у вікні: інакше
# писати CSS для чужої розмітки — гра в вгадайку.
SELECTORS = [
    ("#list", "Уся стрічка",
     "Колонка з повідомленнями: відступи від країв, напрямок і проміжок між рядками.",
     "#list {\n  inset: 16px;      /* відступи від країв вікна */\n  gap: 10px;        /* проміжок між рядками */\n}"),
    (".m", "Одне повідомлення",
     "Обгортка кожного рядка: фон, рамка, скруглення, поля.",
     ".m {\n  background: rgba(0,0,0,.45);\n  padding: 3px 8px;\n  border-radius: 10px;\n}"),
    ('.m[data-platform="twitch"]', "Тільки з Twitch",
     "Те саме, але лише для Twitch. Так само: kick, youtube, site (чат сайту).",
     '.m[data-platform="twitch"] {\n  border-left: 3px solid #9146ff;\n  padding-left: 6px;\n}'),
    ('.m[data-platform="kick"]', "Тільки з Kick",
     "Повідомлення, що прийшли з Kick.",
     '.m[data-platform="kick"] {\n  border-left: 3px solid #53fc18;\n  padding-left: 6px;\n}'),
    ('.m[data-platform="youtube"]', "Тільки з YouTube",
     "Повідомлення, що прийшли з YouTube.",
     '.m[data-platform="youtube"] {\n  border-left: 3px solid #ff0033;\n  padding-left: 6px;\n}'),
    ('.m[data-kind="money"]', "Донат / Super Chat",
     "Рядок із грошима. Поряд працює клас .paid — типове оформлення донату.",
     '.m[data-kind="money"] {\n  background: rgba(251,191,36,.28);\n  border-radius: 10px;\n}'),
    ('.m[data-kind="system"]', "Системне повідомлення",
     "Рейди, підписки, оголошення самої площадки.",
     '.m[data-kind="system"] {\n  opacity: .7;\n}'),
    (".ico", "Значок площадки",
     "Логотип Twitch / Kick / YouTube перед рядком. Розмір в em — тягнеться за текстом.",
     ".ico {\n  width: 1.2em;\n  height: 1.2em;\n}"),
    (".b", "Значок автора",
     "MOD, VIP, SUB, HOST і решта плашок біля ніка.",
     ".b {\n  border-radius: 999px;\n  font-size: .5em;\n}"),
    (".n", "Нік автора",
     "Колір приходить із площадки і стоїть інлайном — свій треба ставити з !important.",
     ".n {\n  color: #ffd166 !important;\n  font-weight: 800;\n}"),
    (".money", "Сума донату",
     "Жовта плашка з сумою.",
     ".money {\n  background: #22c55e;\n  color: #052e16;\n}"),
    (".re", "Кому відповідають",
     "Рядок «↳ нік» перед текстом відповіді.",
     ".re {\n  color: #c4b5fd;\n  font-size: .75em;\n}"),
    (".em", "Емоут",
     "Картинка емоута всередині тексту.",
     ".em {\n  height: 2em;\n}"),
    (".at", "Звертання @нік",
     "Підсвічене звертання в тексті повідомлення.",
     ".at {\n  background: #f59e0b;\n  color: #111;\n}"),
    (".sys", "Текст системного",
     "Курсив рейдів, підписок і подібного.",
     ".sys {\n  font-style: normal;\n  color: #c4b5fd;\n}"),
    (".paid", "Оформлення донату",
     "Жовта смуга ліворуч і підкладка для повідомлень із грошима.",
     ".paid {\n  border-left-color: #22c55e;\n}"),
    ("body", "Загальні налаштування",
     "Шрифт, базовий кегль, колір тексту, тінь під текстом.",
     "body {\n  font-size: 26px;\n  text-shadow: 0 0 4px #000, 0 2px 3px #000;\n}"),
    ("@keyframes in", "Поява рядка",
     "Анімація, з якою новий рядок виїжджає знизу. Можна замінити своєю.",
     "@keyframes in {\n  from { opacity: 0; transform: translateX(-12px); }\n}"),
]

# --- готові рецепти ---------------------------------------------------------
#
# Найчастіше людині потрібно не «дізнатися про клас», а зробити одну конкретну
# річ: збільшити текст, прибрати плашки, підсвітити донати. Тому поруч із
# довідником — список готових шматків: подвійний клік вставляє, наведення
# показує, що саме вставиться.
RECIPES = [
    ("Крупніший текст", "Найчастіша правка: чат у кадрі дрібний.",
     "body { font-size: 26px; }"),
    ("Компактні рядки", "Більше повідомлень в тій самій висоті.",
     "#list { gap: 2px; }\n.m { line-height: 1.15; }"),
    ("Підкладка під рядком", "Читається на будь-якій картинці, не лише на темній.",
     ".m {\n  background: rgba(0,0,0,.5);\n  padding: 3px 8px;\n  border-radius: 10px;\n}"),
    ("Смуга кольору площадки", "Видно з одного погляду, звідки прийшло повідомлення.",
     '.m[data-platform="twitch"] { border-left: 3px solid #9146ff; padding-left: 6px; }\n'
     '.m[data-platform="kick"]   { border-left: 3px solid #53fc18; padding-left: 6px; }\n'
     '.m[data-platform="youtube"]{ border-left: 3px solid #ff0033; padding-left: 6px; }'),
    ("Прибрати значки автора", "MOD/VIP/SUB зникають, лишається нік.",
     ".b { display: none; }"),
    ("Прибрати іконки площадок", "Коли площадка одна, значок лише займає місце.",
     ".ico { display: none; }"),
    ("Свій колір ніків", "Площадка ставить свій колір інлайном — тому !important.",
     ".n { color: #ffd166 !important; font-weight: 800; }"),
    ("Донати помітніше", "Гроші не мають губитися серед звичайних рядків.",
     '.m[data-kind="money"] {\n  background: rgba(251,191,36,.30);\n  border-radius: 10px;\n}\n'
     ".money { font-size: .9em; }"),
    ("Системні тихіше", "Рейди й підписки не перебивають розмову.",
     ".sys { opacity: .55; font-size: .78em; }"),
    ("Без анімації появи", "Якщо рух у кадрі відволікає.",
     ".m { animation: none; }"),
    ("Товстіший контур тексту", "Читається навіть на світлій грі.",
     "body { text-shadow: 0 0 4px #000, 0 0 8px #000, 0 2px 3px #000; }"),
    ("Більші емоути", "Емоути на всю висоту рядка.",
     ".em { height: 2em; }"),
    ("Сховати чат сайту", "Лишити тільки площадки.",
     '.m[data-platform="site"] { display: none; }'),
    ("Яскравіші звертання", "Коли звертаються до вас — має кидатися в очі.",
     ".at { background: #f59e0b; color: #111; border-radius: .3em; }"),
    ("Рядок вліво, а не знизу", "Інша анімація появи.",
     "@keyframes in { from { opacity: 0; transform: translateX(-14px); } }"),
    ("Все праворуч", "Чат притиснутий до правого краю вікна.",
     "#list { align-items: flex-end; text-align: right; }"),
]

# --- приклади для перегляду -------------------------------------------------
#
# Навмисно різні: із значками, з емоутом, з грошима, відповідь, системне,
# звертання. Стилі підбирають саме на такому наборі, а не на одному рядку.
_EMOTE = ("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='28' height='28'>"
          "<circle cx='14' cy='14' r='13' fill='%23fbbf24'/><circle cx='9' cy='11' r='2' fill='%23111'/>"
          "<circle cx='19' cy='11' r='2' fill='%23111'/><path d='M8 18q6 5 12 0' stroke='%23111' "
          "stroke-width='2' fill='none' stroke-linecap='round'/></svg>")

SAMPLES = [
    {"platform": "twitch", "name": "GoodTheme", "nick": "goodtheme", "color": "#ff7f50",
     "badges": ["mod", "sub"], "text": "о, привіт! Kappa як воно?",
     "emotes": [{"code": "Kappa", "url": _EMOTE}]},
    {"platform": "kick", "name": "xQc_fan", "nick": "xqc_fan", "color": "#53fc18",
     "badges": ["vip"], "text": "@lazar1n не забудь про рейд", "reply": "lazar1n"},
    {"platform": "youtube", "name": "Оксана", "nick": "oksana", "color": "#ff6b81",
     "badges": ["member"], "text": "дякую за стрім!", "amount": "200 UAH"},
    {"platform": "twitch", "name": "raid", "kind": "system",
     "text": "GoodTheme рейдить вас — 42 глядачі"},
    {"platform": "site", "name": "lazar1n", "nick": "lazar1n", "color": "#a855f7",
     "badges": ["broadcaster"], "text": "вітаю всіх, поїхали"},
]

# --- перевірка синтаксису ---------------------------------------------------


def validate_css(text: str):
    """Помилки CSS із номерами рядків.

    Це не повний парсер CSS і не має ним бути: браузер мовчки викидає те, чого
    не зрозумів, і людина лишається з питанням «чому не працює». Тут ловиться
    саме те, через що правило зникає цілком: незакрита дужка, коментар чи
    лапки, оголошення без двокрапки, порожній селектор.
    """
    errors = []
    depth = 0
    open_lines = []
    i, line = 0, 1
    n = len(text)
    decl_start_line = 1          # рядок, де почалося поточне оголошення
    buf = []                     # накопичене оголошення (між ; та } )
    at_rule = False

    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        # коментар
        if ch == "/" and text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end == -1:
                errors.append((line, "коментар /* не закрито"))
                break
            line += text.count("\n", i, end)
            i = end + 2
            continue
        # рядок у лапках
        if ch in "\"'":
            end = i + 1
            while end < n and text[end] != ch:
                if text[end] == "\\":
                    end += 1
                elif text[end] == "\n":
                    break
                end += 1
            if end >= n or text[end] != ch:
                errors.append((line, "лапки %s не закрито" % ch))
                break
            i = end + 1
            continue
        if ch == "{":
            selector = "".join(buf).strip()
            if depth == 0 and not selector:
                errors.append((line, "порожній селектор перед {"))
            at_rule = selector.startswith("@")
            depth += 1
            open_lines.append(line)
            buf = []
            decl_start_line = line
            i += 1
            continue
        if ch == "}":
            rest = "".join(buf).strip().strip(";")
            if rest and depth > 0 and not at_rule:
                _check_decl(rest, decl_start_line, errors)
            if depth == 0:
                errors.append((line, "зайва } — блок не було відкрито"))
            else:
                depth -= 1
                open_lines.pop()
            buf = []
            decl_start_line = line
            i += 1
            continue
        if ch == ";":
            decl = "".join(buf).strip()
            if depth > 0 and decl:
                _check_decl(decl, decl_start_line, errors)
            buf = []
            decl_start_line = line
            i += 1
            continue
        if not buf and ch.strip():
            decl_start_line = line
        buf.append(ch)
        i += 1

    if depth > 0:
        errors.append((open_lines[-1] if open_lines else line, "блок { не закрито"))
    tail = "".join(buf).strip()
    if depth == 0 and tail and not tail.startswith("@"):
        errors.append((decl_start_line, "текст поза правилом: «%s»" % _short(tail)))
    return errors


def _check_decl(decl: str, line: int, errors: list):
    if ":" not in decl:
        errors.append((line, "оголошення без двокрапки: «%s»" % _short(decl)))
        return
    prop, value = decl.split(":", 1)
    if not prop.strip():
        errors.append((line, "немає назви властивості перед двокрапкою"))
    elif not value.strip():
        errors.append((line, "властивість «%s» без значення" % prop.strip()))


def _short(text: str, limit: int = 40) -> str:
    text = " ".join(text.split())
    return text if len(text) <= limit else text[:limit] + "…"


# --- редактор з нумерацією рядків ------------------------------------------

class _Gutter(QWidget):
    def __init__(self, editor):
        super().__init__(editor)
        self.editor = editor

    def sizeHint(self):
        return QSize(self.editor.gutter_width(), 0)

    def paintEvent(self, event):
        self.editor.paint_gutter(event)


class CodeEdit(QPlainTextEdit):
    """Моноширинний редактор із номерами рядків і підсвіткою поточного."""

    def __init__(self, parent=None, read_only=False):
        super().__init__(parent)
        font = QFont("Consolas")
        font.setStyleHint(QFont.Monospace)
        font.setPointSize(10)
        self.setFont(font)
        self.setTabStopDistance(4 * self.fontMetrics().horizontalAdvance(" "))
        self.setLineWrapMode(QPlainTextEdit.NoWrap)
        self.setReadOnly(read_only)
        self.gutter = _Gutter(self)
        self.blockCountChanged.connect(lambda _: self._update_margins())
        self.updateRequest.connect(self._on_update)
        self.cursorPositionChanged.connect(self._highlight_line)
        self._update_margins()
        self._highlight_line()

    def gutter_width(self) -> int:
        digits = max(2, len(str(max(1, self.blockCount()))))
        return 12 + self.fontMetrics().horizontalAdvance("9") * digits

    def _update_margins(self):
        self.setViewportMargins(self.gutter_width(), 0, 0, 0)

    def _on_update(self, rect, dy):
        if dy:
            self.gutter.scroll(0, dy)
        else:
            self.gutter.update(0, rect.y(), self.gutter.width(), rect.height())
        if rect.contains(self.viewport().rect()):
            self._update_margins()

    def resizeEvent(self, e):
        super().resizeEvent(e)
        cr = self.contentsRect()
        self.gutter.setGeometry(QRect(cr.left(), cr.top(), self.gutter_width(), cr.height()))

    def _highlight_line(self):
        if self.isReadOnly():
            return
        sel = QTextEdit.ExtraSelection()
        sel.format.setBackground(QColor("#241a2e"))
        sel.format.setProperty(QTextFormat.FullWidthSelection, True)
        sel.cursor = self.textCursor()
        sel.cursor.clearSelection()
        self.setExtraSelections([sel])

    def paint_gutter(self, event):
        painter = QPainter(self.gutter)
        painter.fillRect(event.rect(), QColor("#15121b"))
        block = self.firstVisibleBlock()
        number = block.blockNumber()
        top = self.blockBoundingGeometry(block).translated(self.contentOffset()).top()
        bottom = top + self.blockBoundingRect(block).height()
        cur = self.textCursor().blockNumber()
        while block.isValid() and top <= event.rect().bottom():
            if block.isVisible() and bottom >= event.rect().top():
                painter.setPen(QColor("#a855f7") if number == cur else QColor("#5b5566"))
                painter.drawText(0, int(top), self.gutter.width() - 6,
                                 self.fontMetrics().height(), Qt.AlignRight, str(number + 1))
            block = block.next()
            top = bottom
            bottom = top + self.blockBoundingRect(block).height()
            number += 1

    def goto_line(self, line: int):
        cursor = QTextCursor(self.document().findBlockByNumber(max(0, line - 1)))
        self.setTextCursor(cursor)
        self.centerCursor()
        self.setFocus()


EDITOR_CSS = """
QMainWindow, QWidget { background: #100e14; color: #e7e2df;
                       font: 13px 'Segoe UI', sans-serif; }
QPlainTextEdit { background: #1a1620; border: 1px solid rgba(255,255,255,0.08);
                 border-radius: 6px; color: #e7e2df; selection-background-color: #6d28d9; }
QPushButton { background: rgba(255,255,255,0.07); border: 1px solid rgba(255,255,255,0.10);
              border-radius: 6px; padding: 6px 14px; color: #e7e2df; }
QPushButton:hover { background: rgba(255,255,255,0.13); }
QPushButton#primary { background: #a855f7; border-color: #a855f7; color: #fff; font-weight: 600; }
QPushButton#primary:hover { background: #9333ea; }
QPushButton#danger:hover { background: #7f1d1d; }
QTabWidget::pane { border: 1px solid rgba(255,255,255,0.08); border-radius: 6px; }
QTabBar::tab { background: transparent; padding: 6px 14px; color: #9a9490; }
QTabBar::tab:selected { color: #e7e2df; border-bottom: 2px solid #a855f7; }
QTreeWidget, QListWidget { background: #1a1620; border: 1px solid rgba(255,255,255,0.08);
                           border-radius: 6px; }
QTreeWidget::item, QListWidget::item { padding: 3px 2px; }
QTreeWidget::item:selected, QListWidget::item:selected { background: #3b2a52; }
QHeaderView::section { background: #221c2b; color: #9a9490; border: 0; padding: 5px; }
QLabel#status { padding: 4px 8px; border-radius: 6px; }
QLabel#hint { color: #9a9490; }
QSplitter::handle { background: rgba(255,255,255,0.06); }
QToolTip { background: #1a1620; color: #e7e2df; border: 1px solid #a855f7;
           border-radius: 6px; padding: 6px 8px; }
"""


class CssEditor(QMainWindow):
    """Вікно «свій CSS»: редактор, приклад і довідник в одному місці."""

    def __init__(self, win):
        super().__init__(None)
        self.win = win
        self.setWindowTitle("Hominka — свій CSS для чату")
        self.resize(1180, 760)
        self.setMinimumSize(880, 560)
        self.setStyleSheet(EDITOR_CSS)

        root = QWidget(self)
        outer = QVBoxLayout(root)
        outer.setContentsMargins(12, 10, 12, 10)
        outer.setSpacing(8)
        outer.addLayout(self._toolbar())

        split = QSplitter(Qt.Horizontal, root)
        split.addWidget(self._left())
        split.addWidget(self._right())
        split.setStretchFactor(0, 5)
        split.setStretchFactor(1, 4)
        split.setChildrenCollapsible(False)
        outer.addWidget(split, 1)
        self.setCentralWidget(root)

        # Перегляд оновлюємо не на кожну літеру: перемальовувати сторінку в
        # такт набору — і моргання, і марна робота.
        self._debounce = QTimer(self)
        self._debounce.setSingleShot(True)
        self._debounce.setInterval(350)
        self._debounce.timeout.connect(self.apply_preview)
        self.editor.textChanged.connect(self._debounce.start)

        self.editor.setPlainText(win.custom_css or "")
        self._preview_ready = False
        self.preview.loadFinished.connect(self._on_preview_loaded)
        self.preview.setHtml(chatfeed.page_html(win.custom_css or ""),
                             QUrl("https://stream.svitix.com/"))
        self.validate()

    # --- шапка ---
    def _toolbar(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(8)
        title = QLabel("Свій CSS для чату", self)
        title.setStyleSheet("font: 700 15px 'Segoe UI';")
        row.addWidget(title)

        self.status = QLabel("", self)
        self.status.setObjectName("status")
        row.addWidget(self.status)
        row.addStretch(1)

        reset = QPushButton("Скинути до типових", self)
        reset.setObjectName("danger")
        reset.setToolTip("Прибрати свій CSS — лишиться наше типове оформлення.")
        reset.clicked.connect(self.reset)
        row.addWidget(reset)

        save = QPushButton("Зберегти", self)
        save.setObjectName("primary")
        save.setToolTip("Застосувати до вікна чату і запам'ятати (Ctrl+S).")
        save.clicked.connect(self.save)
        row.addWidget(save)
        return row

    # --- ліва половина: редактор, типовий CSS, довідник ---
    def _left(self) -> QWidget:
        box = QWidget(self)
        lay = QVBoxLayout(box)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(6)

        tabs = QTabWidget(box)
        self.editor = CodeEdit(tabs)
        tabs.addTab(self.editor, "Мій CSS")

        base = CodeEdit(tabs, read_only=True)
        base.setPlainText(_base_css())
        tabs.addTab(base, "Типовий CSS")

        tabs.addTab(self._reference(), "Класи")
        tabs.addTab(self._recipes(), "Приклади")
        lay.addWidget(tabs, 1)

        self.errors = QListWidget(box)
        self.errors.setMaximumHeight(96)
        self.errors.itemActivated.connect(self._goto_error)
        self.errors.itemClicked.connect(self._goto_error)
        lay.addWidget(self.errors)

        hint = QLabel("Правила пишуться поверх типових — досить описати те, що змінюєте. "
                      "Подвійний клік у «Класах» вставляє заготовку.", box)
        hint.setObjectName("hint")
        hint.setWordWrap(True)
        lay.addWidget(hint)
        return box

    def _reference(self) -> QWidget:
        tree = QTreeWidget(self)
        tree.setColumnCount(2)
        tree.setHeaderLabels(["Селектор", "Що це"])
        tree.setRootIsDecorated(False)
        for selector, short, desc, example in SELECTORS:
            item = QTreeWidgetItem([selector, short])
            tip = _tip(short, desc, example)
            item.setToolTip(0, tip)
            item.setToolTip(1, tip)
            item.setData(0, Qt.UserRole, example)
            tree.addTopLevelItem(item)
        tree.setColumnWidth(0, 240)
        tree.itemDoubleClicked.connect(self._insert_snippet)
        return tree

    def _recipes(self) -> QWidget:
        """Готові шматки CSS: подвійний клік вставляє, наведення показує код."""
        tree = QTreeWidget(self)
        tree.setColumnCount(2)
        tree.setHeaderLabels(["Що зробити", "Навіщо"])
        tree.setRootIsDecorated(False)
        for title, why, code in RECIPES:
            item = QTreeWidgetItem([title, why])
            tip = _tip(title, why, code)
            item.setToolTip(0, tip)
            item.setToolTip(1, tip)
            item.setData(0, Qt.UserRole, code)
            tree.addTopLevelItem(item)
        tree.setColumnWidth(0, 230)
        tree.itemDoubleClicked.connect(self._insert_snippet)
        return tree

    # --- права половина: живий перегляд ---
    def _right(self) -> QWidget:
        box = QWidget(self)
        lay = QVBoxLayout(box)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(6)

        cap = QLabel("Перегляд — так це виглядатиме в чаті", box)
        cap.setObjectName("hint")
        lay.addWidget(cap)

        self.preview = QWebEngineView(box)
        self.preview.setMinimumWidth(320)
        # Прозорий фон, як у справжньому вікні чату: інакше під повідомленнями
        # біле полотно, і людина підбирає кольори до фону, якого в кадрі немає.
        self.preview.setAttribute(Qt.WA_TranslucentBackground, True)
        self.preview.page().setBackgroundColor(Qt.transparent)
        # Шахівка під прозорим фоном: у OBS чат лежить поверх картинки, і
        # суцільно чорна підкладка обманювала б щодо прозорості.
        holder = QWidget(box)
        holder.setStyleSheet(
            "background-image:"
            " repeating-linear-gradient(45deg,#1f1f1f 0 12px,#171717 12px 24px);"
            " border:1px solid rgba(255,255,255,0.08); border-radius:6px;")
        hl = QVBoxLayout(holder)
        hl.setContentsMargins(4, 4, 4, 4)
        hl.addWidget(self.preview)
        lay.addWidget(holder, 1)

        row = QHBoxLayout()
        again = QPushButton("Показати приклади ще раз", box)
        again.clicked.connect(self.fill_preview)
        row.addWidget(again)
        row.addStretch(1)
        lay.addLayout(row)
        return box

    # --- дії ---
    def _insert_snippet(self, item, _column):
        """Вставляє готовий шматок у кінець тексту і показує результат."""
        snippet = (item.data(0, Qt.UserRole) or item.text(0)).rstrip()
        cursor = self.editor.textCursor()
        cursor.movePosition(QTextCursor.End)
        prefix = "" if self.css().endswith("\n") or not self.css() else "\n"
        cursor.insertText(prefix + "\n" + snippet + "\n")
        self.editor.setTextCursor(cursor)
        self.editor.centerCursor()
        self.editor.setFocus()
        self.apply_preview()

    def _goto_error(self, item: QListWidgetItem):
        line = item.data(Qt.UserRole)
        if line:
            self.editor.goto_line(int(line))

    def css(self) -> str:
        return self.editor.toPlainText()

    def validate(self) -> bool:
        errors = validate_css(self.css())
        self.errors.clear()
        for line, message in errors:
            item = QListWidgetItem("рядок %d: %s" % (line, message))
            item.setData(Qt.UserRole, line)
            item.setForeground(QColor("#fca5a5"))
            self.errors.addItem(item)
        if errors:
            self.status.setText("%d помилк%s" % (len(errors), "а" if len(errors) == 1 else "и"))
            self.status.setStyleSheet("background:#7f1d1d; color:#fff;")
        elif self.css().strip():
            self.status.setText("синтаксис у порядку")
            self.status.setStyleSheet("background:#14532d; color:#dcfce7;")
        else:
            self.status.setText("типове оформлення")
            self.status.setStyleSheet("background:rgba(255,255,255,0.08); color:#9a9490;")
        return not errors

    def apply_preview(self):
        """Показати поточний CSS у прикладі — навіть якщо він з помилками.

        Саме так поводиться браузер: він викидає незрозуміле правило і малює
        решту. Побачити це — корисніше, ніж не побачити нічого.
        """
        self.validate()
        if self._preview_ready:
            self.preview.page().runJavaScript(chatfeed.apply_css_js(self.css()))

    def fill_preview(self):
        if not self._preview_ready:
            return
        self.preview.page().runJavaScript("window.fts&&fts.clear()")
        for event in SAMPLES:
            self.preview.page().runJavaScript(
                "window.fts&&fts.add(%s)" % json.dumps(event, ensure_ascii=False))

    def _on_preview_loaded(self, ok: bool):
        self._preview_ready = bool(ok)
        if ok:
            self.fill_preview()
            self.apply_preview()

    def save(self):
        if not self.validate():
            # Не забороняємо зберегти зламане: людина може дописати завтра, а
            # браузер однаково викине лише зіпсоване правило. Але мовчати не
            # можна — інакше «чому не працює?» лишиться без відповіді.
            self.status.setText("збережено, але є помилки")
            self.status.setStyleSheet("background:#78350f; color:#fff;")
        self.win.set_custom_css(self.css())

    def reset(self):
        self.editor.setPlainText("")
        self.apply_preview()
        self.win.set_custom_css("")

    def keyPressEvent(self, e):
        if e.key() == Qt.Key_S and e.modifiers() & Qt.ControlModifier:
            self.save()
            return
        if e.key() == Qt.Key_Escape:
            self.close()
            return
        super().keyPressEvent(e)


def _tip(title: str, desc: str, example: str) -> str:
    """Підказка з прикладом коду.

    Саме приклад і потрібен: «клас .n — нік автора» не каже, ЩО з ним робити,
    а рядок «.n { color: #ffd166 !important; }» каже все і одразу.
    """
    code = (example.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace("\n", "<br>").replace(" ", "&nbsp;"))
    return ("<b>%s</b><br>%s<br><br><code style='color:#c9a4ff'>%s</code>"
            "<br><br><i>подвійний клік — вставити</i>"
            % (title, desc, code))


def _base_css() -> str:
    """Типовий CSS сторінки — рівно той, що лежить у chatfeed.PAGE.

    Витягуємо з самої сторінки, а не тримаємо другу копію: копія розійшлася б з
    оригіналом на першій же правці, і людина стилізувала б неіснуючі правила.
    """
    match = re.search(r"<style>(.*?)</style>", chatfeed.PAGE, re.S)
    return (match.group(1).strip() if match else "").replace("\r\n", "\n")
