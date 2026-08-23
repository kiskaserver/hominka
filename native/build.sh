#!/bin/sh
# Збирає overlay.dll та injector.exe у двох розрядностях.
#
# Виклик (усередині контейнера ENTRYPOINT робить це сам):
#     sh build.sh [вихідна_тека]
# За замовчуванням кладе результат у /native/dist, який змонтований у
# native/dist на хості (див. native/build.ps1).
set -e

SRC="${SRC:-/src}"
OUT="${1:-$SRC/dist}"
mkdir -p "$OUT"

# -O2                звичайна оптимізація;
# -s                 прибрати символи — DLL і так неофіційна, зайва вага ні до чого;
# -static ...        вкласти рантайм GCC усередину, щоб на чужій машині не бракувало
#                    libstdc++-6.dll / libgcc_s;
# -Wl,--kill-at      прибрати «@N» з імен експортів stdcall (для 32-біт це важливо);
# -municode          wmain як точка входу (юнікодні аргументи).
COMMON="-O2 -s -static -static-libgcc -static-libstdc++ -Wall -Wextra -Wno-unused-parameter"
DLL_LIBS="-ld3d11 -ldxgi -ld3d9 -lgdi32 -lole32 -luuid"
EXE_LIBS="-lshlwapi"

build() {
    CXX="$1"; ARCH="$2"
    echo ">> $ARCH: overlay.dll"
    $CXX $COMMON -shared \
        "$SRC/overlay/dllmain.cpp" "$SRC/overlay/shaders.cpp" \
        -o "$OUT/overlay-$ARCH.dll" $DLL_LIBS -Wl,--kill-at

    echo ">> $ARCH: injector.exe"
    $CXX $COMMON -municode \
        "$SRC/injector/injector.cpp" \
        -o "$OUT/injector-$ARCH.exe" $EXE_LIBS
}

build x86_64-w64-mingw32-g++ x64
build i686-w64-mingw32-g++   x86

# Тестовий хост (лише x64): крихітна гра-макет на DX11, щоб було в що інжектити
# під час перевірки. У випуск не входить, тому й окремо від build().
echo ">> x64: testhost.exe (для перевірки)"
x86_64-w64-mingw32-g++ -O2 -s -static -municode -mwindows \
    "$SRC/testhost/testhost.cpp" -o "$OUT/testhost-x64.exe" -ld3d11 -ldxgi

echo ""
echo "Готово. У $OUT:"
ls -la "$OUT"
