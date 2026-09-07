"""Перевірка Linux-рендера цілком: канал, вікно, малювання.

Запускається ВСЕРЕДИНІ контейнера (там є Xvfb і xwd):

    xvfb-run -s "-screen 0 1280x720x24" python3 /src/render/linux_smoke.py

Що саме перевіряється, і чому кожне окремо:

  1. Канал. Сокет AF_UNIX замість іменованого каналу Windows, але домовленість
     про кадри та сама. Помилка тут тиха: рендер просто не побачить жодного
     повідомлення й показуватиме порожнечу.

  2. Малювання. Кадр «shot» просить рендер записати поточне полотно в PNG.
     Це доводить увесь шлях: сокет → розкладка → Blend2D → файл.

  3. ВІКНО. Найпідступніше. Вікно може бути створене й показане, а на екрані
     буде сміття — досить помилитися з форматом пікселів чи з візуалом.
     Тому знімаємо корінь X через xwd і дивимося, чи справді там наші кольори.
     xwd читаємо самі: його заголовок — це два десятки 32-бітних чисел, і
     тягнути заради них ImageMagick ні до чого.
"""
import os
import json
import socket
import struct
import subprocess
import sys
import time

SOCK_DIR = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
EXE = "/out/hominka-render-linux"
SHOT = "/tmp/shot.png"

# Прикметний колір: якби ми шукали «щось не чорне», підійшла б будь-яка
# похибка. Шукаємо саме його.
MARK = (255, 0, 128)


def frame(header, blob=b""):
    h = json.dumps(header, ensure_ascii=False).encode("utf-8")
    return struct.pack("<I", len(h)) + h + struct.pack("<I", len(blob)) + blob


def read_xwd(path):
    """XWD → (ширина, висота, функція піксель(x, y) -> (r, g, b)).

    Заголовок XWD — це 25 чисел по 32 біти, і завжди BIG-endian, яка б не була
    машина. Порядок полів (з X11/XWDFile.h): 0 довжина заголовка, 3 глибина,
    4 ширина, 5 висота, 11 біт на піксель, 12 байтів у рядку, 19 кількість
    кольорів (саме 19: під 18 лежить розмір палітри, а під 20 вже ширина
    вікна — сплутати їх легко, і тоді зсув поїде на мегабайти). Одразу за заголовком іде палітра по 12 байтів на колір, і лише
    потім пікселі — без цього зсуву картинка «з'їжджає».
    """
    with open(path, "rb") as f:
        data = f.read()
    f = struct.unpack(">25I", data[0:100])
    header_size, depth, w, h = f[0], f[3], f[4], f[5]
    bpp, bytes_per_line, ncolors = f[11], f[12], f[19]
    if bpp not in (24, 32):
        raise SystemExit("несподівані %d біт на піксель (глибина %d)" % (bpp, depth))
    px = data[header_size + ncolors * 12:]

    def at(x, y):
        o = y * bytes_per_line + x * (bpp // 8)
        b, g, r = px[o], px[o + 1], px[o + 2]
        return (r, g, b)

    return w, h, at


def main():
    if not os.environ.get("DISPLAY"):
        print("немає DISPLAY — запускай через xvfb-run")
        return 2

    # Батьком прикидаємося ми самі: рендер стежить за цим pid і піде за ним.
    pid = os.getpid()
    proc = subprocess.Popen([EXE, "--run", str(pid)],
                            stderr=subprocess.PIPE, text=True)
    path = os.path.join(SOCK_DIR, "hominka-%d.sock" % pid)
    back = os.path.join(SOCK_DIR, "hominka-%d-back.sock" % pid)

    # Чекаємо, доки рендер підніме сокети.
    for _ in range(100):
        if os.path.exists(path) and os.path.exists(back):
            break
        time.sleep(0.05)
    else:
        print("ПОМИЛКА: рендер не створив сокетів")
        proc.kill()
        return 1
    print("сокети на місці: %s" % path)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    b = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    b.connect(back)
    b.setblocking(False)
    print("канал відкрито")

    s.sendall(frame({"t": "layout", "layout": ["ico", "badges", "name", "text"]}))
    # opacity=1.0 просимо явно: типове значення вигляду — 0.94 (як у
    # config.json), і воно зсуває колір на 6%. Ми тут перевіряємо ДОСТАВКУ
    # пікселів у вікно, а не прозорість, тож прибираємо зайву змінну.
    s.sendall(frame({"t": "config", "width": 430, "height": 300, "zoom": 1.0,
                     "x": 40, "y": 40, "opacity": 1.0}))
    # Тло рядка — саме той прикметний колір, який шукатимемо на екрані.
    s.sendall(frame({"t": "css", "css": ".m{background:rgb(255,0,128)}"}))
    for i in range(4):
        s.sendall(frame({"t": "msg", "platform": "twitch", "name": "Тест",
                         "nick": "test", "color": "#a970ff",
                         "text": "рядок номер %d" % i, "badges": ["mod"]}))
    time.sleep(1.5)

    # --- 2. Малювання ------------------------------------------------------
    if os.path.exists(SHOT):
        os.unlink(SHOT)
    s.sendall(frame({"t": "shot", "path": SHOT}))
    for _ in range(60):
        if os.path.exists(SHOT) and os.path.getsize(SHOT) > 0:
            break
        time.sleep(0.05)
    ok_shot = os.path.exists(SHOT) and os.path.getsize(SHOT) > 0
    print("знімок полотна: %s (%d байтів)"
          % ("є" if ok_shot else "НЕМАЄ", os.path.getsize(SHOT) if ok_shot else 0))

    # --- 3. Вікно ----------------------------------------------------------
    info = subprocess.run(["xwininfo", "-root", "-tree"], capture_output=True,
                          text=True).stdout
    have_win = "Hominka chat overlay" in info
    print("вікно у дереві X: %s" % ("є" if have_win else "НЕМАЄ"))

    subprocess.run(["xwd", "-root", "-silent", "-out", "/tmp/root.xwd"], check=True)
    w, h, at = read_xwd("/tmp/root.xwd")
    found = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, bl = at(x, y)
            if abs(r - MARK[0]) <= 12 and abs(g - MARK[1]) <= 12 and abs(bl - MARK[2]) <= 12:
                found += 1
    print("екран %dx%d, пікселів нашого кольору: %d" % (w, h, found))

    # --- прибирання --------------------------------------------------------
    s.sendall(frame({"t": "bye"}))
    time.sleep(0.5)
    if proc.poll() is None:
        proc.kill()
    s.close()
    b.close()

    bad = 0
    if not ok_shot:
        print("ПОМИЛКА: полотно не намалювалося")
        bad += 1
    if not have_win:
        print("ПОМИЛКА: вікна в X немає")
        bad += 1
    # 430x300 із тлом рядків — це тисячі пікселів; беремо кожен другий по обох
    # осях, тож поріг із запасом.
    if found < 500:
        print("ПОМИЛКА: на екрані немає нашого кольору — вікно є, а вмісту не видно")
        bad += 1

    if bad:
        err = proc.stderr.read() if proc.stderr else ""
        if err:
            print("--- журнал рендера:\n%s" % err[-2000:])
        return 1
    print("\nгаразд: канал, малювання і вікно X11 працюють")
    return 0


if __name__ == "__main__":
    sys.exit(main())
