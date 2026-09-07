#!/bin/bash
# Збирає Hominka під Linux і кладе .AppImage у dist/.
#
# Три кроки, і перший тут не випадково: нативний рендер чату збирається В ЦЬОМУ
# ЖЕ образі, а не береться з native/dist. Причина — glibc: образ native/ це
# Debian bookworm (2.36), а вся програма орієнтується на Ubuntu 22.04 (2.35), і
# зібране на новішій glibc на старішій просто не запускається. Архів виглядав
# би готовим, а на 22.04 чат лишався б порожнім.
#
# Чому AppImage, а не zip з одним файлом (як було). Файл усе одно один, але
# AppImage приносить із собою значок і .desktop, тож у меню програм він
# з'являється сам, а не «просто лежить у теці завантажень». Оновлення при цьому
# не змінилося: сервер віддає архів, оновлювач міняє теку цілком.
set -euo pipefail
cd /src

VERSION="$(grep -oP 'APP_VERSION = "\K[^"]+' hominka/version.py)"
echo "[linux] версія $VERSION"

# --- 1. нативний рендер чату ----------------------------------------------
echo "[linux] нативний рендер"
# SRC для build_linux.sh — це корінь native/, а не корінь проєкту:
# усередині він шукає $SRC/render/*.cpp.
SRC=/src/native TPL="${TPL:-/tpl}" sh native/build_linux.sh /src/dist-render
test -x /src/dist-render/hominka-render-linux

# --- 2. сама програма ------------------------------------------------------
echo "[linux] PyInstaller"
python3 -m PyInstaller --noconfirm --clean \
    --distpath dist-linux --workpath build-linux Hominka_one.spec

# --- 3. AppImage -----------------------------------------------------------
APPDIR=/src/build-linux/Hominka.AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin/native"
cp dist-linux/Hominka "$APPDIR/usr/bin/Hominka"
chmod +x "$APPDIR/usr/bin/Hominka"
# Рендер кладемо в native/ поруч із виконуваним файлом: саме там його шукає
# hominka/inject.py:native_dir() у зібраній програмі.
cp dist-render/hominka-render-linux "$APPDIR/usr/bin/native/"
chmod +x "$APPDIR/usr/bin/native/hominka-render-linux"

cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
# Точка входу AppImage. HERE потрібен, бо всередині образу шляхи відносні, а
# програма шукає native/ поруч із собою.
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/Hominka" "$@"
RUN
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/hominka.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Hominka
Comment=Чат стріму поверх гри
Exec=Hominka
Icon=hominka
Categories=Network;Chat;
Terminal=false
DESK
cp "$APPDIR/hominka.desktop" "$APPDIR/usr/share/applications/hominka.desktop" \
    2>/dev/null || { mkdir -p "$APPDIR/usr/share/applications"; \
    cp "$APPDIR/hominka.desktop" "$APPDIR/usr/share/applications/"; }

# Значок: той самий hominka.png, що йде і в Windows-збірку (Hominka_one.spec).
# AppImage хоче PNG у корені AppDir і однойменний із Icon= у .desktop.
cp hominka.png "$APPDIR/hominka.png"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp hominka.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/hominka.png"

mkdir -p dist
OUT="dist/Hominka-${VERSION}-linux64.AppImage"
rm -f "$OUT"
APPDIR="$APPDIR" appimagetool "$APPDIR" "$OUT" >/dev/null
chmod +x "$OUT"
echo "[linux] готово: $OUT"
