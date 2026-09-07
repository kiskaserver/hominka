"""Дві картинки поруч: браузер зліва, нативний рендер справа.

    python native/render/compare.py ref.png out.png side.png

Це і є перевірка етапу 0: нативний рендер має малювати те саме, що QWebEngine.
Розбіжність у висоті виводиться числом — по ній видно, чи не «поповзли»
міжрядкові відступи, чого на око майже не помітно.
"""

import sys

from PySide6.QtGui import QColor, QFont, QImage, QPainter
from PySide6.QtWidgets import QApplication

GAP = 24                       # проміжок між колонками
LABEL_H = 34
BG = QColor(12, 12, 15)


def main():
    left_path = sys.argv[1] if len(sys.argv) > 1 else "ref.png"
    right_path = sys.argv[2] if len(sys.argv) > 2 else "out.png"
    out_path = sys.argv[3] if len(sys.argv) > 3 else "side.png"

    QApplication(sys.argv)
    left = QImage(left_path)
    right = QImage(right_path)
    if left.isNull() or right.isNull():
        print("не читається: %s або %s" % (left_path, right_path))
        return 1

    w = left.width() + GAP + right.width()
    h = LABEL_H + max(left.height(), right.height())
    canvas = QImage(w, h, QImage.Format_RGB32)
    canvas.fill(BG)

    p = QPainter(canvas)
    f = QFont("Segoe UI", 11)
    f.setBold(True)
    p.setFont(f)
    p.setPen(QColor(200, 200, 210))
    p.drawText(8, 22, "QWebEngine (еталон) — %dx%d" % (left.width(), left.height()))
    p.drawText(left.width() + GAP + 8, 22,
               "нативний (litehtml+D2D) — %dx%d" % (right.width(), right.height()))
    p.drawImage(0, LABEL_H, left)
    p.drawImage(left.width() + GAP, LABEL_H, right)
    p.end()
    canvas.save(out_path)

    diff = right.height() - left.height()
    print("порівняння збережено: %s" % out_path)
    print("висота: еталон %d, нативний %d, різниця %+d px (%.1f%%)"
          % (left.height(), right.height(), diff, 100.0 * diff / left.height()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
