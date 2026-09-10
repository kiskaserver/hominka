# Releasing

Releases are published to three channels. A channel is a signed manifest on the
update server; one archive can be promoted from `dev` to `beta` to `stable`
without being rebuilt.

```
https://update.svitix.com/hominka/{stable,beta,dev}.json
https://update.svitix.com/hominka/files/Hominka-<version>-{win64.zip,linux64.AppImage}
```

## Prerequisites

- Docker.
- The release signing key at `.keys/hominka_release.key` (or `HOMINKA_KEY`).
- The server address in `.keys/ssh_host` (or `HOMINKA_SSH`), e.g. `user@host`,
  with key-based SSH access.

`.keys/` is git-ignored. The script refuses to run without a server address
rather than publishing a manifest that has lost its release history.

## Publishing

```sh
python tools/release.py --version 3.2.0 --channel beta --kind minor \
    --notes "What changed, written for the people who will read it"

# the same archive to stable, without rebuilding
python tools/release.py --version 3.2.0 --channel stable --kind minor \
    --notes "…" --reuse
```

The script stamps the version into `native/render/include/core/version.h`,
builds Windows and Linux in Docker, packs the archive, computes SHA-256 and
size, appends the previous manifest's history, signs the manifest and uploads
files first and the manifest last — so an interrupted upload leaves the channel
pointing at the previous, working release.

`--kind` is one of `major`, `minor`, `patch`, `hotfix`. `--mandatory` makes the
app install without asking. `--dry-run` prints the manifest and uploads nothing.

Commit the version bump afterwards as `release: <version>` and tag it
`v<version>`.

## The signed payload

`tools/signing.py` defines the exact bytes that are signed. The app rebuilds the
same bytes in `native/render/src/update/release.cpp`; the two must match
byte-for-byte. Treat the format as frozen: installations already in the field
only know the current one, and would reject every release signed any other way.
The app accepts a list of keys (`kReleaseKeys`), so the key itself can be
rotated by shipping the new public key first.

## After a release

Every build is a new file with no download reputation. If SmartScreen or Chrome
flag it, [false-positives.md](false-positives.md) has ready-to-send texts for
Microsoft and Google.
