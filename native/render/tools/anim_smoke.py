"""Перевірка анімованих емоутів: GIF і WebP.

Три речі, які тут легко зробити тихо неправильно, і кожну видно лише на око:

  1. GIF складається з ЛАТОК. Кадри 2, 3, 4 у файлі — це не картинки, а
     шматки зі своїм зсувом, які накладаються на попередній стан. Хто малює їх
     як самостійні кадри, отримує емоут, що стрибає й обрізається. Тому в
     зразку ліва половина ЗАВЖДИ червона, а міняється лише права: якщо збірка
     кадрів зламана, червона половина зникне.

  2. Анімація не мусить коштувати 60 кадрів на секунду. Емоут на 100 мс має
     перемальовувати стрічку десять разів на секунду, а не шістдесят —
     інакше зникає весь сенс нативного рендера. Рахуємо кадри за час.

  3. Дірка під емоут. Анімований емоут не запікається в картинку рядка, а
     кладеться поверх щокадру. Якби перший кадр таки запікся, крізь наступні
     просвічував би він.

  .venv\\Scripts\\python.exe native\\render\\anim_smoke.py
"""
import base64
import io
import os
import sys
import time

from paths import APP
sys.path.insert(0, APP)

from PIL import Image, ImageDraw  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from hominka.nativerender import NativeRenderer  # noqa: E402

# Ліва половина — незмінна, права крутиться. Кольори навмисно неприродні для
# чату, щоб не сплутати їх із ніком чи плашкою.
LEFT = (255, 0, 128)
CYCLE = [(0, 255, 128), (0, 128, 255), (255, 255, 0), (128, 0, 255)]
DELAY_MS = 100
SIZE = 32


def build(fmt):
    """Анімація з чотирьох кадрів у вигляді data:-адреси."""
    frames = []
    for c in CYCLE:
        im = Image.new("RGB", (SIZE, SIZE), LEFT)
        ImageDraw.Draw(im).rectangle((SIZE // 2, 0, SIZE - 1, SIZE - 1), fill=c)
        frames.append(im)
    buf = io.BytesIO()
    if fmt == "gif":
        # optimize=True змушує PIL писати саме латки — той випадок, заради
        # якого перевірка й існує.
        frames[0].save(buf, format="GIF", save_all=True, append_images=frames[1:],
                       duration=DELAY_MS, loop=0, optimize=True, disposal=1)
        mime = "image/gif"
    else:
        frames[0].save(buf, format="WEBP", save_all=True, append_images=frames[1:],
                       duration=DELAY_MS, loop=0, lossless=True)
        mime = "image/webp"
    data = buf.getvalue()
    return data, "data:%s;base64,%s" % (mime, base64.b64encode(data).decode())


def tag_counts(w, h, data):
    """Скільки пікселів кожного з наших кольорів у кадрі."""
    out = {"left": 0}
    for i in range(len(CYCLE)):
        out[i] = 0
    want = [("left", LEFT)] + list(enumerate(CYCLE))
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            p = row + x * 4
            b, g, r = data[p], data[p + 1], data[p + 2]
            for key, (cr, cg, cb) in want:
                if abs(r - cr) <= 14 and abs(g - cg) <= 14 and abs(b - cb) <= 14:
                    out[key] += 1
                    break
    return out


def run(fmt, app):
    raw, url = build(fmt)
    r = NativeRenderer(None, preview=True)
    if not r.start():
        print("  процес перегляду не піднявся")
        return False
    frames = []
    r.frame.connect(lambda w, h, d: frames.append((w, h, bytes(d), time.time())))
    r.set_config(zoom=1.0, width=300, height=200)
    r.set_layout(["name", "text"])
    r.set_css("body{font-size:20px}.m{background:#111}.em{height:2em}")
    r.message({"platform": "twitch", "name": "A", "nick": "a", "color": "#888888",
               "text": "zzz", "badges": [],
               "emotes": [{"code": "zzz", "url": url}]})

    # Спершу даємо рядку зʼявитися й влягтися.
    until = time.time() + 1.0
    while time.time() < until:
        app.processEvents()
        time.sleep(0.005)
    frames.clear()

    # А тепер міряємо саму анімацію: рівно 2 секунди спокою, під час яких
    # нічого не відбувається, крім обертання емоута.
    t0 = time.time()
    until = t0 + 2.0
    while time.time() < until:
        app.processEvents()
        time.sleep(0.005)
    r.stop()

    print("  %s: кадрів за 2 с — %d" % (fmt.upper(), len(frames)))
    if not frames:
        print("  ПОМИЛКА: емоут не крутиться зовсім")
        return False

    seen = set()
    left_missing = 0
    checked = 0
    for w, h, d, _ in frames:
        c = tag_counts(w, h, d)
        if c["left"] < 40 and max(c[i] for i in range(len(CYCLE))) < 40:
            continue                      # емоута в кадрі ще/вже немає
        checked += 1
        if c["left"] < 40:
            left_missing += 1
        for i in range(len(CYCLE)):
            if c[i] >= 40:
                seen.add(i)

    print("  %s: різних кадрів анімації побачено %d із %d" % (fmt.upper(), len(seen), len(CYCLE)))
    ok = True
    if len(seen) < 3:
        print("  ПОМИЛКА: анімація стоїть (кадри не міняються)")
        ok = False
    # Ліва половина — це і є перевірка складання латок.
    if left_missing:
        print("  ПОМИЛКА: у %d кадрах зникла незмінна половина — кадри складено "
              "неправильно" % left_missing)
        ok = False
    else:
        print("  %s: незмінна половина на місці в усіх %d кадрах" % (fmt.upper(), checked))
    # 4 кадри по 100 мс — це 10 змін на секунду, тобто ~20 за дві. Стеля 60
    # відсікає «малюємо щоразу»: то було б понад 100.
    if len(frames) > 60:
        print("  ПОМИЛКА: %d кадрів замість ~20 — стрічка малюється дарма" % len(frames))
        ok = False
    return ok


def big_emote(seed):
    """Важкий анімований емоут: 128x128 на 24 кадри — 1.5 МБ у розібраному
    вигляді. Саме такі й переповнюють кеш."""
    frames = []
    for k in range(24):
        im = Image.new("RGB", (128, 128), ((seed * 7 + k * 5) % 256, k * 20 % 256, seed % 256))
        ImageDraw.Draw(im).rectangle((k * 8, 0, k * 8 + 20, 127), fill=(255, 255, 255))
        frames.append(im)
    buf = io.BytesIO()
    frames[0].save(buf, format="WEBP", save_all=True, append_images=frames[1:],
                   duration=80, loop=0, lossless=True)
    return "data:image/webp;base64,%s" % base64.b64encode(buf.getvalue()).decode()


def log_evictions():
    """Рядки журналу про вигнання, залишені саме цим запуском."""
    import os as _os
    path = _os.path.join(_os.environ.get("TEMP", "."), "hominka-overlay.log")
    out = []
    try:
        with io.open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "кеш: викинуто" in line:
                    out.append(line.strip().split("render: ")[-1])
    except Exception:
        pass
    return out


def rss_mb(pid):
    """Памʼять процесу в МБ. psutil тут немає, тож питаємо в самої Windows."""
    if not pid:
        return 0
    import subprocess
    try:
        out = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/NH", "/FO", "CSV"],
                             capture_output=True, text=True, encoding="cp866",
                             errors="replace").stdout
    except Exception:
        return 0
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 5 and parts[1].strip() == str(pid):
            digits = "".join(c for c in parts[4] if c.isdigit())
            if digits:
                return int(digits) // 1024
    return 0


def run_lru(app):
    """Багато різних важких емоутів: кеш мусить триматися в межах і не впасти."""
    # 60 штук по 1.5 МБ — це 94 МБ проти межі в 64 МБ. Кількість (60) навмисно
    # НИЖЧА за межу в 64 штуки: так перевіряється саме межа за памʼяттю.
    N = 60
    print("  готую %d важких емоутів (по 1.5 МБ розібраними — разом ~94 МБ)…" % N)
    urls = [big_emote(i) for i in range(N)]

    r = NativeRenderer(None, preview=True)
    if not r.start():
        print("  процес перегляду не піднявся")
        return False
    frames = []
    r.frame.connect(lambda w, h, d: frames.append(1))
    r.set_config(zoom=1.0, width=300, height=200)
    r.set_layout(["name", "text"])
    r.set_css("body{font-size:20px}.m{background:#111}.em{height:2em}")


    for i, u in enumerate(urls):
        r.message({"platform": "twitch", "name": "A", "nick": "a", "color": "#888888",
                   "text": "e%d" % i, "badges": [],
                   "emotes": [{"code": "e%d" % i, "url": u}]})
        # Пауза після КОЖНОГО: рядок, який не встиг потрапити в кадр, навіть
        # не розкладається, а отже його емоут і не розбирається — кеш би
        # просто не наповнився, і перевірка нічого б не перевірила.
        until = time.time() + 0.12
        while time.time() < until:
            app.processEvents()
            time.sleep(0.005)

    until = time.time() + 1.5
    while time.time() < until:
        app.processEvents()
        time.sleep(0.005)

    alive = r.alive()
    rss = rss_mb(getattr(getattr(r, "_proc", None), "pid", None))
    r.stop()

    evicted = log_evictions()
    print("  LRU: кадрів прийшло %d, процес живий: %s, памʼять %s МБ"
          % (len(frames), "так" if alive else "НІ", rss or "?"))
    print("  LRU: у журналі записів про вигнання — %d%s"
          % (len(evicted), (": " + evicted[-1]) if evicted else ""))
    if not evicted:
        print("  ПОМИЛКА: межа не спрацювала жодного разу")
        return False
    if not alive:
        print("  ПОМИЛКА: рендер упав на потоці важких емоутів")
        return False
    if not frames:
        print("  ПОМИЛКА: після %d емоутів стрічка перестала малюватися" % N)
        return False
    # 94 МБ анімацій при межі 64 МБ: без вигнання памʼять пішла б далеко за 150.
    if rss and rss > 190:
        print("  ПОМИЛКА: %d МБ — межа кеша не тримає" % rss)
        return False
    return True


def main():
    app = QApplication([])
    bad = 0
    for fmt in ("gif", "webp"):
        if not run(fmt, app):
            bad += 1
    print("[межа кеша]")
    if not run_lru(app):
        bad += 1
    if bad:
        print("\nне пройшло: %d із 3" % bad)
        return 1
    print("\nгаразд: обидва формати крутяться, латки складаються, дарма не\n"
          "малюємо, і кеш тримається в межах")
    return 0


if __name__ == "__main__":
    sys.exit(main())
