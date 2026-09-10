# native/

Everything that runs natively, built in one Docker image (see
[docs/building.md](../docs/building.md)).

| Directory | What |
|---|---|
| `render/` | the application: chat sources, CSS rendering, the window, settings and CSS editor, updater — Windows and Linux |
| `overlay/` | `overlay.dll`, the in-game overlay: hooks for DX9, DX11, DX12, OpenGL and the Vulkan layer |
| `injector/` | `injector.exe`, loads `overlay.dll` into the chosen game |
| `common/` | shared by all of the above: the capture-excluded window, the shared-frame contract, the anti-cheat guard |
| `testhost/` | minimal DX9 / DX11 / DX12 / OpenGL / Vulkan windows for testing the overlay safely |
| `patches/` | patches applied to third-party sources at build time |

`thirdparty.sh` and `thirdparty_linux.sh` fetch dependencies at pinned revisions;
`build.sh` / `build_linux.sh` run inside the container.

How the pieces fit together: [docs/architecture.md](../docs/architecture.md).
The story of how they came to be: [docs/history/native-port.md](../docs/history/native-port.md).
