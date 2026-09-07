"""Чи запускається САМА програма на Linux.

Досі перевірявся рендер: він малює, показує вікно, говорить каналом. Але
дістатися до нього людині не було чим — Hominka на Linux не стартувала взагалі,
бо три модулі тягли `wintypes` на рівні імпорту.

Тут запускається справжня програма (`hominka.app.main`) під Xvfb, у режимі
нативного рендера. Перевіряємо три речі:

  1. вона не впала — процес живий після старту;
  2. вона підняла рендер — окремий процес hominka-render-linux;
  3. на екрані є вікно;
  4. чат МАЛЮЄТЬСЯ — і саме через програму, а не повз неї.

Четверте окремо, бо перші три можуть пройти й тоді, коли зв'язок між програмою
та рендером розірваний: процеси є, вікна є, а повідомлення нікуди не йдуть.
Тому другим заходом програма піднімається в тому ж процесі, у неї кладуться
події чату тим самим шляхом, яким їх кладе Twitch, і в рендера просять знімок.

Мережі в контейнері немає, тож справжній чат не під'єднається — але він тут і
не потрібен: події ми даємо самі.

    Xvfb :99 -screen 0 1280x720x24 &
    DISPLAY=:99 python3 /src/render/app_linux_smoke.py
"""
import json
import os
import shutil
import subprocess
import sys
import time

SRC = "/src"
BIN = "/out/hominka-render-linux"


def main():
    if not os.environ.get("DISPLAY"):
        print("немає DISPLAY — запускай під Xvfb")
        return 2

    # У dev-режимі програма кладе config.json поруч із пакетом. Пишемо його
    # заздалегідь: інакше вона піде типовим шляхом (браузер), а нас цікавить
    # саме нативний.
    cfg = os.path.join(SRC, "config.json")
    with open(cfg, "w", encoding="utf-8") as f:
        json.dump({"renderer": "native", "twitchChannel": "test",
                   "geometry": {"x": 60, "y": 60, "w": 430, "h": 400},
                   "frameless": False, "autoUpdate": False}, f)

    # Рендер програма шукає поруч із собою; у контейнері він у /out.
    native = os.path.join(SRC, "native")
    os.makedirs(native, exist_ok=True)
    if not os.path.exists(os.path.join(native, "hominka-render-linux")):
        shutil.copy2(BIN, os.path.join(native, "hominka-render-linux"))

    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "xcb"
    env["PYTHONPATH"] = SRC
    proc = subprocess.Popen([sys.executable, "-c",
                             "from hominka import app; app.main()"],
                            cwd=SRC, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)
    time.sleep(8.0)

    alive = proc.poll() is None
    print("програма жива через 8 с: %s" % ("так" if alive else "НІ"))

    ps = subprocess.run(["ps", "-eo", "comm"], capture_output=True, text=True).stdout
    have_render = "hominka-render" in ps
    print("процес рендера піднявся: %s" % ("так" if have_render else "НІ"))

    tree = subprocess.run(["xwininfo", "-root", "-tree"],
                          capture_output=True, text=True).stdout
    wins = [l for l in tree.splitlines() if "Hominka" in l or "hominka" in l]
    print("вікон Hominka на екрані: %d" % len(wins))
    for w in wins[:4]:
        print("   %s" % w.strip()[:110])

    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    out, err = proc.communicate()

    bad = 0
    if not alive:
        print("ПОМИЛКА: програма впала на старті")
        bad += 1
    if not have_render:
        print("ПОМИЛКА: нативний рендер не запустився")
        bad += 1
    if not wins:
        print("ПОМИЛКА: жодного вікна на екрані")
        bad += 1

    # Слід від винятку в журналі — теж поломка, навіть якщо процес вижив.
    trace = "Traceback" in (err or "") or "Traceback" in (out or "")
    if trace:
        print("ПОМИЛКА: у журналі є виняток")
        bad += 1
    if bad or trace:
        if err:
            print("--- stderr:\n%s" % err[-3000:])
        if out:
            print("--- stdout:\n%s" % out[-1500:])
        return 1

    print()
    return draw_check()


def draw_check():
    """Друга частина: події чату через саму програму — і знімок від рендера."""
    sys.path.insert(0, SRC)
    os.chdir(SRC)
    os.environ["QT_QPA_PLATFORM"] = "xcb"

    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication
    from hominka.overlay import Overlay

    app = QApplication([])
    win = Overlay(None)
    win.show()

    # Даємо програмі підняти рендер (вона робить це через QTimer).
    deadline = time.time() + 5.0
    while time.time() < deadline and getattr(win, "_native", None) is None:
        app.processEvents()
        time.sleep(0.02)
    native = getattr(win, "_native", None)
    if native is None:
        print("ПОМИЛКА: програма не створила нативного рендера")
        return 1
    deadline = time.time() + 5.0
    while time.time() < deadline and not native.alive():
        app.processEvents()
        time.sleep(0.02)
    print("рендер піднятий програмою: %s" % ("так" if native.alive() else "НІ"))

    # Тло рядка робимо прикметним — по ньому й будемо впізнавати, що
    # намалювалося саме наше.
    win._push_native({"kind": "css", "css": ".m{background:rgb(0,255,128)}"})
    for i in range(4):
        win._push_native({"platform": "twitch", "name": "Стрімер",
                          "nick": "streamer", "color": "#a970ff",
                          "text": "повідомлення через програму %d" % i,
                          "badges": ["mod"]})
    deadline = time.time() + 3.0
    while time.time() < deadline:
        app.processEvents()
        time.sleep(0.02)

    shot = "/tmp/app_shot.png"
    if os.path.exists(shot):
        os.unlink(shot)
    native.send("shot", path=shot)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        app.processEvents()
        time.sleep(0.05)
        if os.path.exists(shot) and os.path.getsize(shot) > 0:
            break

    ok = os.path.exists(shot) and os.path.getsize(shot) > 0
    print("знімок чату від програми: %s (%d байтів)"
          % ("є" if ok else "НЕМАЄ", os.path.getsize(shot) if ok else 0))
    if ok:
        marked = count_colour(shot, (0, 255, 128))
        print("пікселів нашого тла у знімку: %d" % marked)
        ok = marked > 2000

    try:
        win.close()
    except Exception:
        pass
    if not ok:
        print("ПОМИЛКА: подія дійшла до програми, але чат не намалювався")
        return 1
    print("\nгаразд: Hominka на Linux стартує, піднімає рендер і малює чат")
    return 0


def count_colour(png, want):
    """Скільки пікселів заданого кольору у PNG. Читаємо самі: Pillow в образі
    немає, а PNG тут наш власний — без інтерлейсу й без палітри."""
    import struct
    import zlib
    data = open(png, "rb").read()
    pos, w, h, raw = 8, 0, 0, b""
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif typ == b"IDAT":
            raw += body
        pos += 12 + ln
    px = zlib.decompress(raw)
    stride = w * 4
    found = 0
    prev = bytearray(stride)
    off = 0
    for _ in range(h):
        ft = px[off]
        line = bytearray(px[off + 1:off + 1 + stride])
        off += 1 + stride
        # Розпаковуємо фільтри PNG: без цього «кольори» будуть різницями.
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if ft == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ft == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        for x in range(0, stride, 4):
            if (abs(line[x] - want[0]) <= 12 and abs(line[x + 1] - want[1]) <= 12
                    and abs(line[x + 2] - want[2]) <= 12):
                found += 1
        prev = line
    return found


if __name__ == "__main__":
    sys.exit(main())
