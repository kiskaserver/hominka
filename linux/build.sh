#!/bin/bash
# Збирає Hominka одним файлом під Linux і кладе zip у dist/.
set -euo pipefail
cd /src

VERSION="$(grep -oP 'APP_VERSION = "\K[^"]+' hominka/version.py)"
echo "[linux] версія $VERSION"

python3 -m PyInstaller --noconfirm --clean --distpath dist-linux --workpath build-linux Hominka_one.spec

cd dist-linux
chmod +x Hominka
zip -9 "../dist/Hominka-${VERSION}-linux64.zip" Hominka
echo "[linux] готово: dist/Hominka-${VERSION}-linux64.zip"
