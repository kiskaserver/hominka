# Linux build

The AppImage is built in a container, not on a developer's machine, because on
Linux it matters where a binary was built: it will not start on a system with an
older glibc than the one it was linked against. The image is Ubuntu 22.04 —
the oldest system the AppImage supports.

```sh
docker build -t hominka-linux -f linux/Dockerfile .
docker run --rm -v "$PWD:/src" hominka-linux
```

Output: `dist/Hominka-<version>-linux64.AppImage`, containing one executable
(`usr/bin/Hominka`), its icon and a `.desktop` entry, so it appears in the
application menu. Updates replace the AppImage file itself.

The native core is compiled in this image rather than taken from `native/dist`:
that image is Debian bookworm (glibc 2.36), and a binary from it would not start
on Ubuntu 22.04 (glibc 2.35).

## Checking on a clean system

```sh
docker build -t hominka-appcheck -f linux/Dockerfile.check .
docker run --rm -v "$PWD/dist:/dist:ro" hominka-appcheck
```

Runs the AppImage on a bare Ubuntu 22.04 under Xvfb and waits for the chat
window. The build image has compilers and `-dev` packages that a user's system
does not, so a forgotten runtime dependency would otherwise go unnoticed.

## Why there is no "invisible to OBS" on Linux

On Windows the operating system itself hides the window from capture
(`SetWindowDisplayAffinity` with `WDA_EXCLUDEFROMCAPTURE`). Neither X11 nor
Wayland lets a window forbid being captured.

It matters less there, because on Linux you normally capture the game, not the
screen:

| OBS source | Does the overlay end up in the stream? |
| --- | --- |
| Window Capture (Xcomposite), X11 | no — only the game window is captured |
| Window Capture (PipeWire), Wayland | no |
| obs-vkcapture (Vulkan / OpenGL games) | no |
| Screen Capture / Display Capture | **yes** — the whole screen, overlay included |

Capture the game window, not the screen.
