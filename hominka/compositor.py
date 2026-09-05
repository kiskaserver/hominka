"""Змусити DWM компонувати робочий стіл — щоб чат було видно поверх гри у
безрамковому повноекранному режимі (Hunt: Showdown і подібні).

Проблема (ресерч). Сучасні ігри в «безрамковому повноекранному» йдуть під
Fullscreen Optimizations + flip-моделлю. Коли поверх гри немає жодного
«справжнього» вікна, DWM переводить її свопчейн у Independent Flip / MPO: кадр
гри сканується прямо на екран, ОМИНАЮЧИ композитор, у якому намальоване наше
вікно чату. На завантаженні/в меню незалежного flip немає (2D, є композиція) —
чат видно; у 3D-грі він вмикається — і чат зникає.

Наше вікно чату приховане від захоплення (WDA_EXCLUDEFROMCAPTURE, «захищений
спрайт»). Схоже, DWM НЕ враховує такі вікна, коли вирішує вмикати незалежний
flip, — тобто саме воно композицію не форсує. Тому потрібне ОКРЕМЕ крихітне
вікно, яке з захоплення НЕ виключене: його DWM у рішенні враховує, і поки воно
поверх гри — незалежний flip вимкнено, а отже наш WDA-чат знову компонується і
його видно на моніторі (в ефір він, як і раніше, не потрапляє).

Ключове (з ресерчу ForceComposedFlip): повністю прозоре вікно DWM викидає з
композиції — тому тримаємо суцільний колір із альфою вікна 1/255 (на око
непомітно, 4×4 пікселі в кутку монітора гри), але DWM мусить його компонувати.
"""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QWidget


class CompositionKeeper(QWidget):
    """Майже-невидиме НЕ-приховане вікно, що форсує композицію DWM над грою.

    Показуємо лише коли попереду повноекранна гра (інакше не потрібне і тільки
    лишало б цятку на робочому столі). Клік-крізь, без фокусу, завжди зверху.
    Спеціально позначене (objectName), щоб CaptureGuard НЕ приховав його від
    захоплення — інакше воно втратило б сенс.
    """

    OBJ_NAME = "hominkaCompositionKeeper"

    def __init__(self):
        super().__init__(None,
                         Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
                         | Qt.WindowDoesNotAcceptFocus | Qt.WindowTransparentForInput)
        self.setObjectName(self.OBJ_NAME)
        self.setAttribute(Qt.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WA_TransparentForMouseEvents, True)
        # НЕ WA_TranslucentBackground: нам потрібен намальований піксель, а не
        # повна прозорість (її DWM оптимізує геть). Суцільний колір + альфа вікна
        # 1/255 → на екрані практично невидимо, але композиція форсується.
        self.setStyleSheet("background: #000;")
        self.setWindowOpacity(1.0 / 255.0)
        self.resize(4, 4)

    def place(self, left: int, top: int):
        """Ставить вікно в куток монітора гри й показує (без активації)."""
        self.move(int(left), int(top))
        if not self.isVisible():
            self.show()

    def hide_keeper(self):
        if self.isVisible():
            self.hide()
