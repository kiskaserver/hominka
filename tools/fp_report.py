# -*- coding: utf-8 -*-
"""Готовий текст звернення про хибне спрацювання антивіруса.

Після кожного випуску це та сама рутина: узяти версію, порахувати хеші,
перелічити, що лежить в архіві, і вставити все у форму Microsoft. Руками це
п'ять хвилин копіювання й одна нагода помилитися в хеші, тому — скрипт.

    python tools/fp_report.py                    # останній архів у dist/
    python tools/fp_report.py 3.2.1              # конкретна версія
    python tools/fp_report.py --file шлях.zip

Сам текст — у docs/false-positives.md; тут він живе поруч із фактами, які
підставляються. Якщо міняється поведінка програми (новий ключ реєстру,
новий мережевий адресат) — правити ОБИДВА місця, і починати з документа.
"""
import argparse
import hashlib
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(HERE, "dist")
BASE_URL = "https://update.svitix.com/hominka/files/"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def newest_zip():
    found = []
    for name in os.listdir(DIST):
        m = re.match(r"Hominka-(\d+)\.(\d+)\.(\d+)-win64\.zip$", name)
        if m:
            found.append((tuple(int(x) for x in m.groups()), name))
    if not found:
        raise SystemExit("у dist/ немає жодного Hominka-*-win64.zip")
    return os.path.join(DIST, max(found)[1])


TEXT = """Hominka is an open-source chat overlay for live streamers. It shows Twitch,
Kick and YouTube chat in a transparent always-on-top window, so the streamer
can read chat while playing full-screen games.

Source code: https://github.com/kiskaserver/hominka
Download:    {url}
SHA-256:     {sha}
Size:        {size} bytes

The archive contains:

{files}
Why this is likely flagged:

1. Every release is a freshly built, unsigned native binary with no download
   history. We do not yet have an Authenticode certificate.

2. The optional "chat inside the game" feature does load a DLL into another
   process. This is genuine DLL injection and we understand why heuristics
   react to it: to draw the chat inside a full-screen game's own frame, the
   overlay has to run in that game's process, the same way the Discord, Steam
   and RTSS overlays do. It is off by default; the user turns it on, picks the
   game window and confirms a warning. It never runs unattended or at startup.

3. The window hides itself from screen capture (WDA_EXCLUDEFROMCAPTURE) so the
   chat does not appear in the stream. That is the point of the product, not
   evasion: the chat is visible on the streamer's own monitor at all times.

4. It writes three registry values, all under HKEY_CURRENT_USER. We list them
   because a sandbox sees them anyway, and they look worse than they are:

     Software\\Classes\\hominka
       Its own URL scheme, so a button on hominka.app can hand a chat theme to
       the app (hominka://theme/<name>). Written on start only when the value
       differs from what is already there.

     Software\\Khronos\\Vulkan\\ImplicitLayers
       Registers our Vulkan layer, and only while the in-game overlay is on:
       that layer is how a Vulkan game gets the overlay at all. Removed again
       when the feature is switched off or the app exits.

     Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers
       Only when the user presses "Remove fullscreen optimisation" for a game
       they picked themselves, and we only REMOVE the
       DISABLEDXMAXIMIZEDWINDOWEDMODE flag there, never add it.

   None of these is a startup entry or a way to run code unattended.

What the program does NOT do: it does not install anything, does not write to
system directories, does not add startup entries, does not run as
administrator, does not collect or send user data anywhere. Settings live in
%LOCALAPPDATA%\\Hominka\\config.json. Network traffic goes only to Twitch, Kick,
YouTube, the streamer's own site, the emote services (7TV, BetterTTV,
FrankerFaceZ) and our update server (update.svitix.com).

Updates are signed with our own Ed25519 key and verified before installation,
so a tampered update cannot be installed even if our server were compromised.

Microsoft Defender with current signatures finds nothing in these files
locally (MpCmdRun.exe -Scan -ScanType 3). The verdict we see on VirusTotal is
Trojan:Win32/Wacatac.B!ml — the cloud model, on a file that is new, unsigned
and injects a DLL by design.

The whole build is reproducible in Docker (native/Dockerfile), and we are glad
to provide anything else that helps.
"""


def main():
    ap = argparse.ArgumentParser(description="текст звернення про хибне спрацювання")
    ap.add_argument("version", nargs="?", help="версія, напр. 3.2.1")
    ap.add_argument("--file", help="конкретний архів")
    args = ap.parse_args()

    if args.file:
        path = args.file
    elif args.version:
        path = os.path.join(DIST, "Hominka-%s-win64.zip" % args.version)
    else:
        path = newest_zip()
    if not os.path.isfile(path):
        raise SystemExit("немає файлу: %s" % path)

    with zipfile.ZipFile(path) as z:
        names = sorted(i.filename for i in z.infolist() if not i.is_dir())
    files = "".join("  %s\n" % n for n in names) + "\n"

    print(TEXT.format(url=BASE_URL + os.path.basename(path), sha=sha256(path),
                      size=os.path.getsize(path), files=files))
    print("-" * 70, file=sys.stderr)
    print("Форма: https://www.microsoft.com/en-us/wdsi/filesubmission", file=sys.stderr)
    print("       Software developer → Incorrectly detected as malware", file=sys.stderr)
    print("Архів: %s" % path, file=sys.stderr)


if __name__ == "__main__":
    main()
