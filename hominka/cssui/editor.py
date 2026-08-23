"""Вікно «свій CSS»: редактор, живий приклад і довідник в одному місці."""

import json
import re

from PySide6.QtCore import Qt, QTimer, QUrl
from PySide6.QtGui import QColor, QIcon, QTextCursor
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QListWidget, QListWidgetItem, QMainWindow, QPushButton,
    QSizeGrip, QSplitter, QTabWidget, QTreeWidget, QTreeWidgetItem, QVBoxLayout,
    QWidget,
)
from PySide6.QtWebEngineWidgets import QWebEngineView

from .. import feed as chatfeed
from ..paths import resource_path
from ..version import APP_ICON
from .catalog import RECIPES, SAMPLES, SELECTORS
from .codeedit import CodeEdit
from .styles import EDITOR_CSS
from .validator import validate_css

class CssEditor(QMainWindow):
    """Вікно «свій CSS»: редактор, приклад і довідник в одному місці."""

    def __init__(self, win):
        super().__init__(None)
        self.win = win
        # Назва без «Hominka —»: її додає смужка вікна, а в панелі завдань
        # програма й так підписана своїм іменем.
        self.setWindowTitle("Hominka — свій CSS для чату")
        self.setWindowIcon(QIcon(resource_path(APP_ICON)))
        self.resize(1180, 760)
        self.setMinimumSize(880, 560)
        self.setStyleSheet(EDITOR_CSS)
        # Без системної рамки: вікно чату й панель налаштувань свої, і чуже
        # синє обрамлення посеред них виглядало як інша програма.
        self.setWindowFlags(Qt.Window | Qt.FramelessWindowHint)
        self._drag_from = None

        root = QWidget(self)
        root.setObjectName("root")
        outer = QVBoxLayout(root)
        outer.setContentsMargins(12, 8, 12, 10)
        outer.setSpacing(8)
        outer.addLayout(self._titlebar())
        outer.addLayout(self._toolbar())

        split = QSplitter(Qt.Horizontal, root)
        split.addWidget(self._left())
        split.addWidget(self._right())
        split.setStretchFactor(0, 5)
        split.setStretchFactor(1, 4)
        split.setChildrenCollapsible(False)
        outer.addWidget(split, 1)
        self.setCentralWidget(root)
        # Рамки немає — тягнути за край нічим, тож куточок ставимо самі.
        self._grip = QSizeGrip(root)
        self._grip.setFixedSize(16, 16)

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

    # --- своя смужка вікна ---
    def _titlebar(self) -> QHBoxLayout:
        """Іконка, назва і хрестик — як у вікні чату.

        Рамку малюємо самі, тож і тягнути вікно доводиться самим: обробники
        миші нижче.
        """
        row = QHBoxLayout()
        row.setSpacing(8)

        icon = QLabel(self)
        icon.setPixmap(QIcon(resource_path(APP_ICON)).pixmap(18, 18))
        row.addWidget(icon)

        name = QLabel("Hominka", self)
        name.setStyleSheet("font:700 13px 'Segoe UI'; color:#e7e2df;")
        row.addWidget(name)

        sub = QLabel("свій CSS для чату", self)
        sub.setStyleSheet("font:12px 'Segoe UI'; color:#8f8a96;")
        row.addWidget(sub)
        row.addStretch(1)

        close = QPushButton("✕", self)
        close.setObjectName("close")
        close.setFixedSize(26, 24)
        close.setToolTip("Закрити (Esc)")
        close.clicked.connect(self.close)
        row.addWidget(close)
        return row

    # --- перетягування вікна за смужку ---
    def mousePressEvent(self, e):
        if e.button() == Qt.LeftButton and e.position().y() < 44:
            self._drag_from = e.globalPosition().toPoint() - self.frameGeometry().topLeft()
        super().mousePressEvent(e)

    def mouseMoveEvent(self, e):
        if self._drag_from is not None and e.buttons() & Qt.LeftButton:
            self.move(e.globalPosition().toPoint() - self._drag_from)
        super().mouseMoveEvent(e)

    def mouseReleaseEvent(self, e):
        self._drag_from = None
        super().mouseReleaseEvent(e)

    # --- шапка ---
    def _toolbar(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(8)

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

    def resizeEvent(self, e):
        super().resizeEvent(e)
        self._grip.move(self.width() - self._grip.width() - 4,
                        self.height() - self._grip.height() - 4)
        self._grip.raise_()

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
