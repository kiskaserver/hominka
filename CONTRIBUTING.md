# Contributing

Thanks for wanting to make Hominka better. Bug reports with a clear way to
reproduce them are the most valuable contribution of all — see the issue
templates.

## Before you start

For anything bigger than a small fix, open an issue first and describe what you
want to change. Hominka runs next to games on streamers' machines; changes to
the in-game overlay, the injector or the update path get the most scrutiny.

## Building and testing

See [docs/building.md](docs/building.md). Everything builds in Docker on any
host. For the in-game overlay use the test hosts in `native/dist/`
(`testhost-{x64,dx9-x64,dx12-x64,gl-x64,vk-x64}.exe`) — never test injection
on a real online game.

Before sending a change, build both platforms and check it in the running app,
not only that it compiles. For UI changes, include a screenshot.

## Conventions

- **Comments explain why.** The codebase's comments are written in Ukrainian
  and record the reason behind a decision — usually the bug or the dead end
  that led to it. Keep that density and that focus in new code; English
  comments are welcome too.
- **Commits** are `area: what changed`, e.g. `window: locked chat passes
  clicks through`. Common areas: `render`, `ui`, `window`, `net`, `game`,
  `update`, `build`, `linux`, `release`. The body says why.
- **Match the surrounding code** — naming, idiom, error handling. No new
  dependencies without discussing them first; every dependency is pinned in
  `native/thirdparty*.sh` and listed in `THIRD_PARTY_NOTICES.md`.

## Things that must not change casually

- The signed payload format (`tools/signing.py` ↔ `update/release.cpp`).
  Installed copies only accept the current format.
- The shared-frame contract (`native/common/shared_frame.h`) between the app and
  `overlay.dll` / the Vulkan layer.
- The anti-cheat block list in `native/common/guard.h` only ever grows.

## License

By contributing you agree that your contribution is licensed under the
[GNU GPL v3.0](LICENSE), the license of the project.
