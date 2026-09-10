# Third-party notices

Hominka is distributed under the [GNU General Public License v3.0](LICENSE).
It is built from the following third-party components. None of them is vendored
in this repository: `native/thirdparty.sh` and `native/thirdparty_linux.sh`
fetch each one at a pinned revision inside the build container, so the version
listed here is exactly the version that ships.

| Component | Version | License | Used for | Platform |
|---|---|---|---|---|
| [litehtml](https://github.com/litehtml/litehtml) | v0.10 | BSD-3-Clause | HTML/CSS layout of chat messages | all |
| [Gumbo](https://github.com/google/gumbo-parser) (bundled with litehtml) | — | Apache-2.0 | HTML parsing | all |
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.92.9b | MIT | window chrome, settings, CSS editor | all |
| [FreeType](https://freetype.org) | 2.13.3 | FreeType License (FTL) | glyph rasterisation for the UI | all |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 3.6.7 | Apache-2.0 | TLS, SHA-256 | all |
| [IXWebSocket](https://github.com/machinezone/IXWebSocket) | v12.0.1 | BSD-3-Clause | WebSocket chat connections | all |
| [Monocypher](https://monocypher.org) | 4.0.3 | BSD-2-Clause OR CC0-1.0 | Ed25519 release signature verification | all |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | MIT | JSON | all |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | v1.5.0 | BSD-3-Clause | animated WebP emotes | all |
| [NanoSVG](https://github.com/memononen/nanosvg) | `239e102` | zlib | SVG badges and icons | all |
| [Blend2D](https://blend2d.com) | `58ca946` | zlib | 2D rendering | Linux |
| [AsmJit](https://asmjit.com) | `4d46552` | zlib | Blend2D JIT pipeline | Linux |
| [SDL](https://libsdl.org) | 2.30.9 | zlib | settings windows | Linux |
| [stb](https://github.com/nothings/stb) | `2c980bb` | MIT OR Unlicense | image decoding | Linux |

On Windows, Direct2D, DirectWrite, DirectComposition, Direct3D and WIC are
system components and are not redistributed. Fonts (Segoe UI on Windows, the
system sans-serif found through fontconfig on Linux) are loaded from the
operating system and are likewise not redistributed.

The full license texts are published by each project at the links above; the
Apache-2.0-licensed components (Mbed TLS, Gumbo) are used unmodified and carry
no NOTICE file of their own.
