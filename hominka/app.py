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


def _version_channel() -> str:
    """«2.7.1-beta» — версія цієї копії плюс канал з її config.json.

    Канал читаємо з файлу конкретної копії, а не з архіву: тоді бета й стабільна
    з однакового архіву все одно показують РІЗНЕ (у стрімера часто стоять поруч),
    а сам архів лишається спільним для обох каналів (жодних колізій імен на
    сервері). До появи вікна це єдине, що каже, ЩО саме запускається."""
    try:
        import json
        from .paths import CONFIG_PATH
        ch = "stable"
        if os.path.isfile(CONFIG_PATH):
            with open(CONFIG_PATH, encoding="utf-8") as f:
                ch = (json.load(f).get("channel") or "stable")
        return "%s-%s" % (APP_VERSION, ch)
    except Exception:
        return APP_VERSION


def _install_crash_log():
    """Записує НЕОБРОБЛЕНІ винятки у %TEMP%\\hominka-overlay.log (той самий журнал,
    що й нативна частина) — інакше в зібраній програмі без консолі краш не лишає
    жодного сліду. Не заважає стандартній поведінці (кличемо оригінальний хук)."""
    import traceback, tempfile
    from datetime import datetime
    prev = sys.excepthook

    def hook(exctype, value, tb):
        try:
            p = os.path.join(tempfile.gettempdir(), "hominka-overlay.log")
            with open(p, "a", encoding="utf-8") as f:
                f.write("[%s] НЕОБРОБЛЕНИЙ ВИНЯТОК:\n"
                        % datetime.now().strftime("%H:%M:%S.%f")[:-3])
                traceback.print_exception(exctype, value, tb, file=f)
        except Exception:
            pass
        prev(exctype, value, tb)

    sys.excepthook = hook


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else None
    _install_crash_log()
    os.environ.setdefault("QTWEBENGINE_CHROMIUM_FLAGS", "--enable-features=TranslucentWindows")
    hide_internal_folder()
    badge = _version_channel()
    splash_text("%s · запускаю…" % badge, 92)
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
    splash_text("%s · відкриваю чат…" % badge, 97)
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
