# Security policy

## Reporting a vulnerability

Please **do not** open a public issue for a security problem. Use
[GitHub private vulnerability reporting](https://github.com/kiskaserver/hominka/security/advisories/new)
instead. You will get an answer within a few days; fixes ship as a hotfix on
every release channel at once.

Only the latest release on each channel (stable, beta, dev) is supported.

## What is worth reporting

Hominka runs on a streamer's machine next to a game, so these matter most:

- **The update path.** Anything that makes the app install code that was not
  signed with the release key — a way around the signature check, a manifest
  field that is trusted without being signed, a downgrade.
- **The in-game overlay.** The injector, `overlay.dll` and the Vulkan layer run
  inside another process. Memory-safety bugs there, or a way for a third party
  to make the injector load something other than Hominka's own overlay.
- **Chat input.** Messages are rendered from untrusted text: HTML/CSS injection
  that escapes the message sandbox, crashes caused by crafted messages, emotes or
  badges.
- **Capture exclusion.** A way for the chat to end up in the stream while
  "hidden from OBS" is on.

## How updates are protected

Every release manifest is signed with an Ed25519 key that never leaves the
maintainer's machine; the public key is compiled into the app
(`kReleaseKeys` in `native/render/src/update/release.cpp`). The app verifies the signature over
a canonical payload (`tools/signing.py` is the reference implementation) and the
SHA-256 of the downloaded archive before installing anything. A compromised
download server can therefore withhold updates, but cannot ship code.
