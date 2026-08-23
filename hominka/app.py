"""Запуск програми: QApplication, вікно, глобальні дрібниці."""

import os
import sys

from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QApplication

from .overlay import Overlay
from .paths import resource_path
from .splash import splash_text
from .version import APP_AUTHOR, APP_ICON, APP_NAME, APP_VERSION
from .winapi import CaptureGuard, IS_WINDOWS, hide_internal_folder


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else None
    os.environ.setdefault("QTWEBENGINE_CHROMIUM_FLAGS", "--enable-features=TranslucentWindows")
    hide_internal_folder()
    splash_text("Запускаю…", 92)
    app = QApplication(sys.argv)
    # Фільтр подій усього застосунку ставимо ТІЛЬКИ у Windows, і не з обережності.
    #
    # Він потрібен рівно для одного — сховати кожне нове вікно від захоплення
    # екрана, чого поза Windows не буває. А коштує він дорого: такий фільтр
    # викликається на КОЖНУ подію КОЖНОГО об'єкта, і PySide мусить збудувати
    # пітонівську обгортку навіть для внутрішніх об'єктів Qt. У Linux частина
    # їх приходить із потоків QtWebEngine — і програма падала з SIGSEGV просто
    # у циклі подій (стек: sendThroughApplicationEventFilters →
    # PySide::getWrapperForQObject).
    if IS_WINDOWS:
        # До створення вікон: інакше перше з них з'явиться незахищеним.
        guard = CaptureGuard(app)
        app.installEventFilter(guard)
    app.setApplicationName(APP_NAME)
    # setApplicationDisplayName НЕ ставимо: Qt дописує його до назви кожного
    # вікна, і «Hominka — свій CSS» перетворювалося на
    # «Hominka — свій CSS — Hominka».
    app.setApplicationVersion(APP_VERSION)
    app.setOrganizationName(APP_AUTHOR)
    app.setWindowIcon(QIcon(resource_path(APP_ICON)))
    # Виходимо разом із вікном ЧАТУ — і більше ні з чим (Overlay.closeEvent
    # кличе quit() сам).
    #
    # Типове правило Qt «вийти, коли закрилося останнє вікно» тут працювало
    # проти нас: вікно чату оголошене як Qt.Tool, а такі вікна Qt у цьому
    # підрахунку не бачить. Виходило, що редактор CSS — єдине «справжнє» вікно
    # програми, і його хрестик гасив увесь чат посеред ефіру.
    app.setQuitOnLastWindowClosed(False)
    splash_text("Відкриваю чат…", 97)
    win = Overlay(url)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    if not IS_WINDOWS:
        print("[chat-overlay] Linux: вікно НЕ ховається від захоплення — такого "
              "вміння немає ні в X11, ні у Wayland. Знімайте в OBS не весь "
              "екран, а гру: Window Capture (Xcomposite / PipeWire) або "
              "obs-vkcapture. Тоді оверлей у кадр не потрапляє.")
    main()
