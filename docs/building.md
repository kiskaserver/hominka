# Building

Everything builds inside Docker, on any host. You do not need Visual Studio,
the Windows SDK or a Linux toolchain: Windows binaries are cross-compiled with
mingw-w64 on Debian bookworm, and the Linux AppImage is built on Ubuntu 22.04 —
its glibc is the oldest the AppImage has to run on, and a binary built against a
newer glibc does not start on an older one. Third-party
libraries are fetched at pinned revisions while the image builds (see
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).

The first build downloads and compiles the dependencies and takes several
minutes; after that Docker's layer cache makes rebuilds take about as long as
compiling Hominka itself.

## Windows

```sh
docker build -t hominka-native native
id=$(docker create hominka-native)
docker cp "$id:/out/." native/dist
docker rm "$id"
```

On Windows, `native\build.ps1` does exactly this.

`native/dist/` then contains:

| File | What it is |
|---|---|
| `hominka-render-x64.exe` | the application |
| `injector-{x64,x86}.exe`, `overlay-{x64,x86}.dll` | the optional in-game overlay |
| `hominka-vklayer-{x64,x86}.dll` + `.json` | Vulkan implicit layer for the same |
| `testhost-*.exe` | minimal DX9 / DX11 / DX12 / OpenGL / Vulkan windows for testing the in-game overlay |

> **Why not a bind mount?** Sources are copied into the image with `COPY`, and
> artifacts are taken out with `docker cp`. A bind mount from a Windows host
> serves files through a cache that can lag behind the disk, and more than once
> it quietly compiled yesterday's code.

## Linux

```sh
docker build -t hominka-linux -f linux/Dockerfile .
docker run --rm -v "$PWD:/src" hominka-linux
```

The AppImage lands in `dist/Hominka-<version>-linux64.AppImage`. The version is
read from `native/render/include/core/version.h`, the single source of truth.

To check it the way a user gets it — on a clean Ubuntu 22.04 with nothing but a
display server — run:

```sh
docker build -t hominka-appcheck -f linux/Dockerfile.check .
docker run --rm -v "$PWD/dist:/dist:ro" hominka-appcheck
```

It starts the AppImage under Xvfb and waits for the chat window. CI runs the
same check on every build.

## Checking a build without the UI

The app has a few command-line modes that exercise real code paths without
opening a window:

```sh
hominka-render-x64.exe --selftest native/render/samples.json out.png  # render sample messages to a PNG
hominka-render-x64.exe --csslint theme.css                            # check a custom theme
hominka-render-x64.exe --nettest twitch <channel> 30                  # read a live chat for 30 s
hominka-render-x64.exe --updatecheck beta 3.0.0                       # what a 3.0.0 on beta would be offered
hominka-render-x64.exe --verifyrelease manifest.json                  # verify a manifest signature offline
```

`native/render/tools/release_smoke.py` tampers with a real signed manifest one
field at a time and checks that every change is rejected.

## Line endings

`.gitattributes` keeps `*.sh`, `*.patch` and Dockerfiles at LF on every
platform. Do not override it: with `core.autocrlf=true` a CRLF shell script
copied into the image makes `dash` fail with `Illegal option -`.
