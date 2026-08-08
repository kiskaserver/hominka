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

HERE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(HERE, "dist", "Hominka")

# Куди кладемо. Змінюється лише разом із сервером, тому не в аргументах.
SSH_HOST = os.environ.get("HOMINKA_SSH", "user@your-server")
REMOTE_DIR = os.environ.get("HOMINKA_DIR", "/opt/stream/updates/hominka")
BASE_URL = "https://update.svitix.com/hominka/"

KINDS = ["major", "minor", "patch", "hotfix"]
CHANNELS = ["stable", "beta", "dev"]
HISTORY_LEN = 10  # скільки минулих випусків тримаємо в маніфесті


def run(cmd, **kw):
    print("$", " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def build_exe():
    """PyInstaller. Версію в .exe бере з version_info.txt — його оновлюємо теж."""
    py = os.path.join(HERE, ".venv", "Scripts", "python.exe")
    if not os.path.isfile(py):
        py = sys.executable
    run([py, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed",
         "--name", "Hominka", "--icon", "hominka.ico",
         "--version-file", "version_info.txt", "--add-data", "hominka.ico;.",
         "chat_overlay.py"], cwd=HERE)


def stamp_version(version: str):
    """Проставляє версію в chat_overlay.py і version_info.txt.

    Одне джерело правди — аргумент --version: інакше в маніфесті одне, у вікні
    «про програму» друге, а у властивостях .exe третє."""
    nums = re.findall(r"\d+", version)[:3]
    while len(nums) < 3:
        nums.append("0")
    tup = "(%s, 0)" % ", ".join(nums)

    p = os.path.join(HERE, "chat_overlay.py")
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


def pack(version: str) -> str:
    """Пакує ВМІСТ dist/Hominka (без зайвої теки зверху) — так оновлювач просто
    кладе архів поверх встановленої програми."""
    if not os.path.isfile(os.path.join(DIST, "Hominka.exe")):
        raise SystemExit("немає %s — спершу зберіть (без --no-build)" % DIST)
    out = os.path.join(tempfile.gettempdir(), "Hominka-%s-win64.zip" % version)
    if os.path.exists(out):
        os.remove(out)
    print("пакую", out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for root, _dirs, files in os.walk(DIST):
            for name in files:
                full = os.path.join(root, name)
                z.write(full, os.path.relpath(full, DIST))
    return out


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


def main():
    ap = argparse.ArgumentParser(description="Випуск Hominka на update.svitix.com")
    ap.add_argument("--version", required=True)
    ap.add_argument("--channel", required=True, choices=CHANNELS)
    ap.add_argument("--kind", required=True, choices=KINDS)
    ap.add_argument("--notes", required=True, help="що змінилося — це побачить користувач")
    ap.add_argument("--mandatory", action="store_true",
                    help="ставити наполегливо (для термінових виправлень)")
    ap.add_argument("--no-build", action="store_true", help="взяти вже зібране в dist/")
    ap.add_argument("--reuse", action="store_true",
                    help="архів цієї версії вже на сервері — лише переписати маніфест каналу")
    ap.add_argument("--dry-run", action="store_true", help="нічого не завантажувати")
    args = ap.parse_args()

    name = "Hominka-%s-win64.zip" % args.version
    url = BASE_URL + "files/" + name

    manifest = {
        "product": "hominka",
        "channel": args.channel,
        "version": args.version,
        "kind": args.kind,
        "mandatory": bool(args.mandatory),
        "releasedAt": date.today().isoformat(),
        "notes": args.notes,
        "file": {"platform": "win64", "url": url},
    }

    if args.reuse:
        # Просування вже випущеного архіву в інший канал: розмір і хеш беремо
        # з маніфесту, де він уже є, — перезбирати заради цього нічого не треба.
        for ch in CHANNELS:
            old = fetch_manifest(ch)
            if old.get("version") == args.version and old.get("file", {}).get("sha256"):
                manifest["file"].update({k: old["file"][k] for k in ("size", "sha256")})
                break
        if "sha256" not in manifest["file"]:
            raise SystemExit("--reuse: не знайшов уже випущену версію %s" % args.version)
        zip_path = ""
    else:
        stamp_version(args.version)
        if not args.no_build:
            build_exe()
        zip_path = pack(args.version)
        manifest["file"]["size"] = os.path.getsize(zip_path)
        manifest["file"]["sha256"] = sha256_of(zip_path)
        print("розмір %.1f МБ, sha256 %s" % (manifest["file"]["size"] / 1e6,
                                             manifest["file"]["sha256"][:16] + "…"))

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
    if zip_path:
        # Спершу файл, потім маніфест: якщо перервати посередині, канал ще
        # вказує на стару (робочу) версію, а не на архів, якого немає.
        run(["scp", zip_path, "%s:%s/files/%s" % (SSH_HOST, REMOTE_DIR, name)])
    run(["scp", local_manifest, "%s:%s/%s.json" % (SSH_HOST, REMOTE_DIR, args.channel)])
    print("\nГотово: %s%s.json → %s" % (BASE_URL, args.channel, args.version))

    if zip_path:
        shutil.copy(zip_path, os.path.join(HERE, "dist", name))
        print("копія архіву: dist/%s" % name)


if __name__ == "__main__":
    main()
