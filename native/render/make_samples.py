"""Готує вхідний файл для самоперевірки нативного рендера.

    python native/render/make_samples.py native/render/samples.json

Бере зразки з hominka/cssui/catalog.py — тобто ті самі, що показує редактор
свого CSS. Джерело одне: додали зразок у довідник — він одразу перевіряється й
у нативному рендері.
"""

import io
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from hominka.cssui.catalog import SAMPLES
from hominka.feed.page import DEFAULT_LAYOUT


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "samples.json"
    css = sys.argv[2] if len(sys.argv) > 2 else ""
    data = {"width": 430, "zoom": 1.0, "css": css,
            "layout": DEFAULT_LAYOUT, "messages": SAMPLES}
    io.open(out, "w", encoding="utf-8").write(json.dumps(data, ensure_ascii=False, indent=1))
    print("зразків: %d → %s" % (len(SAMPLES), out))


if __name__ == "__main__":
    main()
