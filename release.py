"""
Випуск нової версії Hominka на update.svitix.com.

Що робить: збирає .exe (за потреби), пакує теку в zip, рахує sha256 і розмір,
оновлює маніфест каналу разом з історією і кладе все на сервер.

    python release.py --version 1.1.0 --channel stable --kind minor \
        --notes "Панель реакцій, автооновлення"

    python release.py --version 1.1.1 --channel stable --kind hotfix \
        --notes "Виправлено вилітання на старті" --mandatory

    python release.py --version 1.2.0-dev --channel dev --kind minor \
        --notes "Пробне" --no-build      (використати вже зібране в dist/)

Канали — це просто три маніфести поруч. Один і той самий архів можна виставити
спершу в dev, потім у beta і врешті в stable: --reuse бере вже завантажений
файл і лише переписує маніфест каналу.

Розкладка на сервері:

    /opt/stream/updates/hominka/
        stable.json  beta.json  dev.json
        files/Hominka-1.1.0-win64.zip

Каталог віддає Caddy на update.svitix.com (див. Caddyfile).
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from datetime import date

from hominka import signing

HERE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(HERE, "dist")
EXE = os.path.join(DIST, "Hominka.exe")

# Куди кладемо. Змінюється лише разом із сервером, тому не в аргументах.
SSH_HOST = os.environ.get("HOMINKA_SSH", "user@your-server")
REMOTE_DIR = os.environ.get("HOMINKA_DIR", "/opt/stream/updates/hominka")
BASE_URL = "https://update.svitix.com/hominka/"

# Приватний ключ підпису випусків. Поруч із репозиторієм, а не в ньому: у git
# йому не місце ніколи. Без нього випуск не збереться — і це правильно,
# непідписане оновлення програма 2.0+ не поставить.
KEY_PATH = os.environ.get("HOMINKA_KEY", os.path.join(HERE, ".keys", "hominka_release.key"))

# Що людина має прочитати ПЕРЕД тим, як перейти в цей канал, — незалежно від
# того, що саме в конкретному випуску. Поки в каналі живе експериментальна
# можливість, вона мусить бути тут: інакше про неї дізнаються, лише коли вона
# спрацює.
_INJECTOR_NOTE = (
    "У розділі ⚙ → «Поверх гри» є «Справжній чат у грі»: Hominka вкладає власну "
    "бібліотеку в процес гри й малює чат усередині кадру — з позицією й розміром "
    "самого вікна чату, стилем у реальному часі, і (для DirectX 12) прихованням "
    "від OBS. Вимкнено за замовчуванням. Бібліотека непідписана — Windows може "
    "перепитати (кнопка «Що потрібно…» пояснює, що дозволити). В ОНЛАЙН-іграх з "
    "античитом інжект НЕ можна — для них є безрамковий режим.")
CHANNEL_WARNINGS = {
    "beta": "Бета-канал: можливі шорсткості. " + _INJECTOR_NOTE,
    "stable": _INJECTOR_NOTE,
}

KINDS = ["major", "minor", "patch", "hotfix"]
CHANNELS = ["stable", "beta", "dev"]
HISTORY_LEN = 10  # скільки минулих випусків тримаємо в маніфесті


def run(cmd, **kw):
    print("$", " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def build_linux(version: str) -> str:
    """Збирає Linux-версію в контейнері й повертає шлях до архіву.

    Робиться в тому ж випуску, що й Windows, і за замовчуванням — саме щоб
    версії не розходилися. Розійдуться вони тихо: людина на Linux просто
    лишиться на старій, не знаючи, що вийшла нова.
    """
    out = os.path.join(DIST, "Hominka-%s-linux64.zip" % version)
    run(["docker", "build", "-q", "-t", "hominka-linux", "-f", "linux/Dockerfile", "."], cwd=HERE)
    run(["docker", "run", "--rm", "-v", "%s:/src" % HERE.replace("\\", "/"), "hominka-linux"], cwd=HERE)
    if not os.path.isfile(out):
        raise SystemExit("контейнер не залишив %s" % out)
    return out


def build_native() -> str:
    """Збирає overlay.dll + injector.exe (обидві розрядності) у контейнері.

    Той самий mingw-w64, що й для Linux. Джерела компілюються під час docker
    build, готове дістаємо через docker cp — без bind-mount, який на Windows
    віддає стару копію (на цьому вже горіли, див. native/README.md).
    """
    ndir = os.path.join(HERE, "native")
    out = os.path.join(ndir, "dist")
    run(["docker", "build", "-t", "hominka-native", ndir], cwd=HERE)
    cid = subprocess.run(["docker", "create", "hominka-native"],
                         check=True, capture_output=True, text=True).stdout.strip()
    try:
        os.makedirs(out, exist_ok=True)
        # Прибираємо старі бінарі ПЕРЕД docker cp: якщо файл заблоковано
        # (лишився тестовий процес із вкладеним оверлеєм), перезапис тихо не
        # відбувається і в архів їде стейл — саме так 2.4.9 повезла старий
        # overlay-x64.dll. Видалення заблокованого впаде голосно, а не мовчки.
        for f in os.listdir(out):
            fp = os.path.join(out, f)
            if os.path.isfile(fp):
                try:
                    os.remove(fp)
                except OSError as e:
                    raise SystemExit("не можу оновити %s (зайнятий іншим "
                                     "процесом? закрий тести з оверлеєм): %s" % (fp, e))
        run(["docker", "cp", "%s:/out/." % cid, out])
    finally:
        subprocess.run(["docker", "rm", cid], capture_output=True)
    need = ["injector-x64.exe", "injector-x86.exe", "overlay-x64.dll", "overlay-x86.dll",
            "hominka-vklayer-x64.dll", "hominka-vklayer-x86.dll"]
    missing = [n for n in need if not os.path.isfile(os.path.join(out, n))]
    if missing:
        raise SystemExit("нативна збірка не дала: %s" % ", ".join(missing))
    # Перевіряємо, що overlay-*.dll — СВІЖІ й наші: у файлі має бути маркер
    # (той самий, що звіряє інжектор). Так ловимо випадок, коли docker cp не зміг
    # перезаписати заблокований файл (напр. лишився запущений тест із вкладеним
    # оверлеєм) і в dist/ застряг старий білд без маркера — саме через це 2.4.9
    # поїхала зі стейлним overlay-x64.dll, і інжектор його відхиляв.
    marker = b"HOMINKA-OVERLAY-D7A1F3E9-b2c4-4a6e-9f10-chat-in-game"
    for dll in ("overlay-x64.dll", "overlay-x86.dll"):
        with open(os.path.join(out, dll), "rb") as f:
            if marker not in f.read():
                raise SystemExit(
                    "%s без маркера — імовірно docker cp не перезаписав "
                    "заблокований файл (закрий тестові процеси з оверлеєм і "
                    "перезбери)" % dll)
    return out


def build_splash(version: str, channel: str):
    """Перемальовує splash.png із версією-каналом (напр. «2.7.0-beta») у куті.

    Заставку видно ВЕСЬ час розпаковування, тож людина одразу бачить, що саме
    запускається — зручно, коли поруч стоять стабільна й бета. Не критично:
    якщо перемалювати не вдалось (немає Pillow тощо), лишаємо стару splash.png і
    йдемо далі — випуск через це зривати не варто."""
    py = os.path.join(HERE, ".venv", "Scripts", "python.exe")
    if not os.path.isfile(py):
        py = sys.executable
    try:
        run([py, "make_splash.py", version, channel], cwd=HERE)
    except SystemExit:
        print("УВАГА: не вдалося перемалювати splash.png — беру наявну")


def build_exe():
    """PyInstaller за Hominka_one.spec — збірка ОДНИМ файлом.

    Раніше збирали текою: поруч із .exe лежала тека _internal на 340 МБ. Тепер
    усе всередині Hominka.exe, а заставку (splash) і відкидання зайвих мовних
    файлів описує сам spec. Версію .exe бере з version_info.txt — його
    оновлюємо теж."""
    py = os.path.join(HERE, ".venv", "Scripts", "python.exe")
    if not os.path.isfile(py):
        py = sys.executable
    run([py, "-m", "PyInstaller", "--noconfirm", "--clean", "Hominka_one.spec"], cwd=HERE)


def stamp_version(version: str):
    """Проставляє версію в hominka/version.py і version_info.txt.

    Одне джерело правди — аргумент --version: інакше в маніфесті одне, у вікні
    «про програму» друге, а у властивостях .exe третє."""
    nums = re.findall(r"\d+", version)[:3]
    while len(nums) < 3:
        nums.append("0")
    tup = "(%s, 0)" % ", ".join(nums)

    p = os.path.join(HERE, "hominka", "version.py")
    src = open(p, encoding="utf-8").read()
    src = re.sub(r'APP_VERSION = "[^"]*"', 'APP_VERSION = "%s"' % version, src, count=1)
    open(p, "w", encoding="utf-8", newline="\n").write(src)

    p = os.path.join(HERE, "version_info.txt")
    src = open(p, encoding="utf-8").read()
    src = re.sub(r"filevers=\([^)]*\)", "filevers=" + tup, src, count=1)
    src = re.sub(r"prodvers=\([^)]*\)", "prodvers=" + tup, src, count=1)
    src = re.sub(r"'FileVersion', '[^']*'", "'FileVersion', '%s'" % version, src, count=1)
    src = re.sub(r"'ProductVersion', '[^']*'", "'ProductVersion', '%s'" % version, src, count=1)
    open(p, "w", encoding="utf-8", newline="\n").write(src)
    print("версію проставлено:", version)


def pack(version: str, exe_path: str = "", suffix: str = "win64",
         native_dir: str = "") -> str:
    """Кладе один файл програми в zip; за потреби — і нативні файли поруч.

    Раніше пакували цілу теку і мусили пильнувати, щоб у неї не потрапили
    config.json і тека profile — тобто чужі налаштування й куки входу. Тепер
    програма — один файл, і пакувати більше нічого: те, що лежить поруч,
    належить користувачу і в оновлення не їде за визначенням.

    Виняток — інжектор (native/): overlay.dll та injector.exe кладемо в підтеку
    native/ поруч із програмою. Тільки для тестових каналів: у стабільній
    непідписана DLL нікому не потрібна і лише псувала б репутацію в SmartScreen
    активним стрімерам, які цієї можливості не вмикають.
    """
    exe_path = exe_path or EXE
    if not os.path.isfile(exe_path):
        raise SystemExit("немає %s — спершу зберіть (без --no-build)" % exe_path)
    out = os.path.join(tempfile.gettempdir(), "Hominka-%s-%s.zip" % (version, suffix))
    if os.path.exists(out):
        os.remove(out)
    print("пакую", out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        z.write(exe_path, os.path.basename(exe_path))
        if native_dir:
            for name in ("injector-x64.exe", "injector-x86.exe",
                         "overlay-x64.dll", "overlay-x86.dll",
                         "hominka-vklayer-x64.dll", "hominka-vklayer-x86.dll"):
                p = os.path.join(native_dir, name)
                if os.path.isfile(p):
                    z.write(p, "native/" + name)
                    print("  + native/%s" % name)
    return out


def sign_manifest(manifest: dict):
    """Підписує випуск приватним ключем і кладе підпис у маніфест."""
    if not os.path.isfile(KEY_PATH):
        raise SystemExit(
            "немає ключа підпису: %s\n"
            "Без нього випуск не поставиться на машини з версією 2.0+.\n"
            "Якщо ключ загублено — згенеруйте новий і випустіть версію, яка знає обидва." % KEY_PATH)
    with open(KEY_PATH, encoding="utf-8") as f:
        secret = signing.unb64(f.read().strip())
    manifest["signature"] = signing.b64(signing.sign(secret, signing.release_payload(manifest)))
    # Перевіряємо власний підпис одразу: помилку тут видно розробнику, а не
    # користувачу, у якого оновлення просто не поставиться.
    if not signing.verify(signing.public_key(secret),
                          signing.unb64(manifest["signature"]),
                          signing.release_payload(manifest)):
        raise SystemExit("підпис не проходить власну перевірку — випуск скасовано")
    print("підписано ключем %s…" % signing.b64(signing.public_key(secret))[:12])


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_manifest(channel: str) -> dict:
    """Читає поточний маніфест каналу з сервера (щоб доповнити історію)."""
    try:
        out = subprocess.run(
            ["ssh", SSH_HOST, "cat %s/%s.json 2>/dev/null || true" % (REMOTE_DIR, channel)],
            check=True, capture_output=True, text=True).stdout.strip()
        return json.loads(out) if out else {}
    except Exception:
        return {}


def entry(version: str, zip_path: str, platform: str) -> dict:
    """Опис одного файлу випуску для маніфесту."""
    return {
        "platform": platform,
        "url": BASE_URL + "files/" + os.path.basename(zip_path),
        "size": os.path.getsize(zip_path),
        "sha256": sha256_of(zip_path),
    }


def main():
    ap = argparse.ArgumentParser(description="Випуск Hominka на update.svitix.com")
    ap.add_argument("--version", required=True)
    ap.add_argument("--channel", required=True, choices=CHANNELS)
    ap.add_argument("--kind", required=True, choices=KINDS)
    ap.add_argument("--notes", required=True, help="що змінилося — це побачить користувач")
    ap.add_argument("--mandatory", action="store_true",
                    help="ставити наполегливо (для термінових виправлень)")
    ap.add_argument("--warning", default="",
                    help="попередження, яке людина мусить прочитати ПЕРЕД встановленням; "
                         "без нього береться стандартне для каналу (CHANNEL_WARNINGS)")
    ap.add_argument("--no-warning", action="store_true",
                    help="випустити без попередження, навіть якщо канал має стандартне")
    ap.add_argument("--no-build", action="store_true", help="взяти вже зібране в dist/")
    ap.add_argument("--no-native", action="store_true",
                    help="не вкладати інжектор навіть у тестовий канал")
    ap.add_argument("--linux-zip", default="",
                    help="готовий архів для Linux; за замовчуванням збираємо самі в контейнері")
    ap.add_argument("--no-linux", action="store_true",
                    help="випустити без Linux-збірки (версії розійдуться — лише якщо інакше ніяк)")
    ap.add_argument("--reuse", action="store_true",
                    help="архів цієї версії вже на сервері — лише переписати маніфест каналу")
    ap.add_argument("--dry-run", action="store_true", help="нічого не завантажувати")
    args = ap.parse_args()

    manifest = {
        "product": "hominka",
        "channel": args.channel,
        "version": args.version,
        "kind": args.kind,
        "mandatory": bool(args.mandatory),
        "releasedAt": date.today().isoformat(),
        "notes": args.notes,
    }
    # Попередження каналу чіпляється саме́ — це не забудькуватість, а те, що
    # забути його один раз уже вийшло: 2.2.2 поїхала в бету без згадки про
    # RTSS, і люди побачили випуск без жодного слова про те, що в каналі є
    # експериментальна можливість. Прапорець --warning перебиває стандартне,
    # --no-warning прибирає його зовсім.
    warning = args.warning or ("" if args.no_warning else CHANNEL_WARNINGS.get(args.channel, ""))
    if warning:
        manifest["warning"] = warning

    uploads = []          # (локальний файл, ім'я на сервері)
    files = []            # записи для маніфесту

    if args.reuse:
        # Просування вже випущеного архіву в інший канал: розміри й хеші беремо
        # з маніфесту, де вони вже є, — перезбирати заради цього нічого не треба.
        for ch in CHANNELS:
            old = fetch_manifest(ch)
            if old.get("version") != args.version:
                continue
            files = old.get("files") or ([old["file"]] if old.get("file") else [])
            if files:
                break
        if not files:
            raise SystemExit("--reuse: не знайшов уже випущену версію %s" % args.version)
    else:
        stamp_version(args.version)
        # Linux — ПЕРШИМ: контейнер бере версію з уже проставленого коду, і
        # робити це після Windows-збірки означало б збирати різні речі.
        linux_zip = args.linux_zip
        if not linux_zip and not args.no_linux:
            linux_zip = build_linux(args.version)
        if not args.no_build:
            build_splash(args.version, args.channel)   # версія-канал на заставці
            build_exe()
        # Інжектор («чат у грі») тепер їде в УСІ канали — це повноцінна
        # можливість, вимкнена за замовчуванням і з попередженням. Збираємо його
        # тим самим контейнером; --no-native дає випуск без нього.
        native_dir = ""
        if not args.no_native:
            native_dir = build_native()
        win_zip = pack(args.version, EXE, "win64", native_dir)
        uploads.append(win_zip)
        files.append(entry(args.version, win_zip, "win64"))
        if linux_zip:
            if not os.path.isfile(linux_zip):
                raise SystemExit("немає %s" % linux_zip)
            uploads.append(linux_zip)
            files.append(entry(args.version, linux_zip, "linux64"))
        elif args.no_linux:
            print("УВАГА: випуск без Linux-збірки — версії розійдуться")
        for f in files:
            print("%s: %.1f МБ, sha256 %s…" % (f["platform"], f["size"] / 1e6, f["sha256"][:16]))

    manifest["files"] = files
    # "file" лишаємо заради вже встановлених програм: вони знають тільки його.
    # Там завжди Windows — саме такі збірки й ходили по цьому полю раніше.
    win = next((f for f in files if f["platform"] == "win64"), None)
    if win:
        manifest["file"] = win

    sign_manifest(manifest)

    # Історія: попередній випуск каналу зсувається вниз.
    prev = fetch_manifest(args.channel)
    history = prev.get("history") or []
    if prev.get("version") and prev.get("version") != args.version:
        history.insert(0, {k: prev.get(k, "") for k in ("version", "kind", "notes", "releasedAt")})
    manifest["history"] = history[:HISTORY_LEN]

    local_manifest = os.path.join(tempfile.gettempdir(), "%s.json" % args.channel)
    with open(local_manifest, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    print(json.dumps(manifest, ensure_ascii=False, indent=2))

    if args.dry_run:
        print("--dry-run: нічого не завантажую")
        return

    run(["ssh", SSH_HOST, "mkdir -p %s/files" % REMOTE_DIR])
    # Спершу файли, потім маніфест: якщо перервати посередині, канал ще вказує
    # на стару (робочу) версію, а не на архів, якого немає.
    for path in uploads:
        run(["scp", path, "%s:%s/files/%s" % (SSH_HOST, REMOTE_DIR, os.path.basename(path))])
    run(["scp", local_manifest, "%s:%s/%s.json" % (SSH_HOST, REMOTE_DIR, args.channel)])
    print()
    print("Готово: %s%s.json → %s" % (BASE_URL, args.channel, args.version))

    # Копія архіву поруч — зручність, а не частина випуску: на сервері він уже
    # лежить. Тому невдача тут не має виглядати як провалений випуск.
    for path in uploads:
        try:
            shutil.copy(path, os.path.join(DIST, os.path.basename(path)))
            print("копія архіву: dist/%s" % os.path.basename(path))
        except OSError as e:
            print("копію в dist/ зробити не вийшло (%s) — випуск це не зачіпає" % e)


if __name__ == "__main__":
    main()
