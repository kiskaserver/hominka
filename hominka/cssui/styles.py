"""Оформлення вікна редактора."""

EDITOR_CSS = """
QMainWindow, QWidget { background: #100e14; color: #e7e2df;
                       font: 13px 'Segoe UI', sans-serif; }
/* Своя рамка замість системної — той самий фіолетовий контур, що у вікна
   чату й панелі налаштувань. */
QWidget#root { border: 2px solid #a855f7; border-radius: 11px; }
/* Хрестик — як у вікні чату. Padding обнуляємо навмисно: загальне правило
   QPushButton нижче дає 6px 14px, і в кнопці 26x24 символ виштовхувало за
   межі — закривати редактор доводилося навпомацки. */
QPushButton#close { background: rgba(255,255,255,0.06); border: 0;
                    border-radius: 6px; padding: 0; color: #e4e4e7;
                    font: 12px 'Segoe UI'; }
QPushButton#close:hover { background: #dc2626; color: #fff; }
QPushButton#close:pressed { background: #b91c1c; }
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
/* Галочка у вкладці «Порядок» має виглядати як галочка, яку можна натиснути:
   без рамки її приймали за значок «увімкнено» і не чіпали. */
QListWidget::indicator { width: 15px; height: 15px; margin-right: 6px;
                         border: 1px solid rgba(255,255,255,0.28);
                         border-radius: 4px; background: rgba(255,255,255,0.04); }
QListWidget::indicator:checked { background: #a855f7; border-color: #a855f7; }
QListWidget::indicator:hover { border-color: #a855f7; }
QTreeWidget::item:selected, QListWidget::item:selected { background: #3b2a52; }
QHeaderView::section { background: #221c2b; color: #9a9490; border: 0; padding: 5px; }
QLabel#status { padding: 4px 8px; border-radius: 6px; }
QLabel#hint { color: #9a9490; }
QSplitter::handle { background: rgba(255,255,255,0.06); }
QToolTip { background: #1a1620; color: #e7e2df; border: 1px solid #a855f7;
           border-radius: 6px; padding: 6px 8px; }
"""
