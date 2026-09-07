"""Один журнал на всі частини оверлея.

Частин у нього чотири й живуть вони в різних процесах: Hominka (Python),
рендер чату, overlay.dll усередині гри та інжектор. Коли щось не працює,
розбиратися в трьох файлах — це шукати не там; тому всі пишуть в один
`%TEMP%\\hominka-overlay.log` і кожен підписується, хто він.
"""

import os
from datetime import datetime

LOG_NAME = "hominka-overlay.log"


def log(msg: str):
    try:
        path = os.path.join(os.environ.get("TEMP", "."), LOG_NAME)
        with open(path, "a", encoding="utf-8") as f:
            f.write("[%s] hominka: %s\n"
                    % (datetime.now().strftime("%H:%M:%S.%f")[:-3], msg))
    except Exception:
        # Журнал не має права зупинити програму: не пишеться — і не треба.
        pass
