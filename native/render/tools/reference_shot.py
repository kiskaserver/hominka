"""Еталонний знімок стрічки — тим самим QWebEngine, що й у програмі.

Навіщо. Нативний рендер (native/render) має малювати те саме, що зараз малює
браузер: на ці класи написані теми користувачів. «Схоже» тут не годиться —
потрібно покласти дві картинки поруч і дивитися. Цей скрипт робить ліву.

    python native/render/reference_shot.py native/render/ref.png

Бере ті самі зразки (hominka/cssui/catalog.py:SAMPLES) і ту саму сторінку
(hominka/feed/page.py), що й самоперевірка нативного рендера, тож розбіжність
на картинці — це справді розбіжність рендерів, а не різні вхідні дані.
"""

import json
import os
import sys

from paths import APP
sys.path.insert(0, APP)

from PySide6.QtCore import QTimer, QUrl, Qt
from PySide6.QtGui import QColor, QImage, QPainter
from PySide6.QtWidgets import QApplication
from PySide6.QtWebEngineWidgets import QWebEngineView

from hominka.cssui.catalog import SAMPLES
from hominka.feed.page import page_html

WIDTH = 430
# Висота з запасом: сторінка притискає рядки до низу, тож зайве лишається
# порожнім згори, а не обрізає повідомлення.
HEIGHT = 1600
BACKDROP = QColor(18, 20, 24)      # те саме темне тло, що й у самоперевірки


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "ref.png"
    css = sys.argv[2] if len(sys.argv) > 2 else ""

    # Chromium не малює вікна, якого не видно, тож вікно РЕАЛЬНО показуємо, але
    # за межами екрана — той самий прийом, що в hominka/gameoverlay.py.
    os.environ.setdefault("QTWEBENGINE_CHROMIUM_FLAGS", "--enable-features=TranslucentWindows")
    app = QApplication(sys.argv)

    view = QWebEngineView()
    view.setAttribute(Qt.WA_TranslucentBackground, True)
    view.page().setBackgroundColor(Qt.transparent)
    view.resize(WIDTH, HEIGHT)
    view.move(-4000, -4000)
    view.show()
    view.setHtml(page_html(css), QUrl("https://stream.svitix.com/"))

    def feed():
        for s in SAMPLES:
            view.page().runJavaScript("window.fts&&fts.add(%s)" % json.dumps(s, ensure_ascii=False))
        # Емоути й значки — це <img> з мережі та data:, їм треба встигнути
        # намалюватися; знімок одразу після add() дав би порожні квадрати.
        QTimer.singleShot(2500, shot)

    def shot():
        img = view.grab().toImage().convertToFormat(QImage.Format_ARGB32)
        canvas = QImage(img.size(), QImage.Format_RGB32)
        canvas.fill(BACKDROP)
        p = QPainter(canvas)
        p.drawImage(0, 0, img)
        p.end()
        canvas.save(out)
        print("еталон збережено: %s (%dx%d)" % (out, canvas.width(), canvas.height()))
        app.quit()

    view.loadFinished.connect(lambda ok: QTimer.singleShot(300, feed) if ok else app.quit())
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
