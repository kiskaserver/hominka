"""Пастки для підпису випуску: що має пройти, а що — ні.

Підпис — це те єдине, що стоїть між людиною і чужим кодом на її машині, тож
перевіряється він не на вигаданому прикладі, а на СПРАВЖНЬОМУ маніфесті з
сервера: беремо його, міняємо по одному полю й дивимося, чи ловиться підміна.

Заразом це перевірка того, що формат підписуваного рядка збігається з
hominka/signing.py до байта: маніфести підписані вже давно, і найменша
розбіжність означала б, що жодне наявне оновлення більше не встановлюється.

    python release_smoke.py [шлях до hominka-render-x64.exe]
"""

import copy
import json
import os
import subprocess
import sys
import tempfile
import urllib.request

from paths import EXE as DEFAULT_EXE
MANIFEST_URL = "https://update.svitix.com/hominka/stable.json"


def fetch():
    req = urllib.request.Request(MANIFEST_URL, headers={"User-Agent": "Hominka-Updater"})
    return json.loads(urllib.request.urlopen(req, timeout=25).read().decode("utf-8"))


def check(exe, data):
    """→ (код виходу, перший рядок відповіді)."""
    with tempfile.NamedTemporaryFile("w", suffix=".json", encoding="utf-8",
                                     delete=False) as f:
        json.dump(data, f, ensure_ascii=False)
        path = f.name
    try:
        r = subprocess.run([exe, "--verifyrelease", path], capture_output=True, timeout=30)
        out = (r.stdout or b"").decode("utf-8", "replace").strip()
        return r.returncode, out.splitlines()[0] if out else ""
    finally:
        os.unlink(path)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    if not os.path.isfile(exe):
        print("не знайшов %s" % exe)
        return 1
    try:
        real = fetch()
    except Exception as e:
        print("не дістав маніфест: %s" % e)
        return 1

    # (назва, що зробити з маніфестом, чи має пройти)
    cases = []

    def case(name, mutate, should_pass):
        cases.append((name, mutate, should_pass))

    case("справжній маніфест", lambda d: d, True)

    def bump_version(d):
        d["version"] = "9.9.9"
        return d
    case("підмінили версію", bump_version, False)

    def swap_url(d):
        d["files"][0]["url"] = "https://update.svitix.com/hominka/files/чуже.zip"
        return d
    case("підмінили посилання", swap_url, False)

    def swap_sha(d):
        d["files"][0]["sha256"] = "0" * 64
        return d
    case("підмінили контрольну суму", swap_sha, False)

    def swap_size(d):
        d["files"][0]["size"] = 123
        return d
    case("підмінили розмір", swap_size, False)

    def swap_channel(d):
        d["channel"] = "dev"
        return d
    case("підмінили канал", swap_channel, False)

    def drop_sig(d):
        d.pop("signature", None)
        return d
    case("прибрали підпис", drop_sig, False)

    def break_sig(d):
        d["signature"] = "A" + d["signature"][1:]
        return d
    case("зіпсували підпис", break_sig, False)

    # Регістр суми в підпис іде зведений до нижнього — тож ВЕЛИКИМИ літерами
    # той самий маніфест мусить лишитися дійсним.
    def upper_sha(d):
        d["files"][0]["sha256"] = d["files"][0]["sha256"].upper()
        return d
    case("сума ВЕЛИКИМИ літерами", upper_sha, True)

    # Це навмисно НЕ підписується: історія й дата переписуються при кожному
    # наступному випуску каналу, і підпис ламався б ні через що.
    def rewrite_notes(d):
        d["notes"] = "щось інше"
        d["releasedAt"] = "2000-01-01"
        d["history"] = []
        return d
    case("змінили опис, дату й історію", rewrite_notes, True)

    # Посилання поза нашим доменом не качаємо навіть із дійсним підписом:
    # маніфест теж приїхав із мережі.
    def foreign_host(d):
        d["files"][0]["url"] = "https://example.com/Hominka.zip"
        return d
    case("посилання на чужий домен", foreign_host, False)

    bad = 0
    for name, mutate, should_pass in cases:
        rc, line = check(exe, mutate(copy.deepcopy(real)))
        passed = rc == 0
        if passed != should_pass:
            bad += 1
            print("НЕ ТАК  %-32s %s" % (name, line))
        else:
            print("гаразд  %-32s %s" % (name, line))

    print("")
    print("пасток: %d, невдалих: %d" % (len(cases), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
