#!/bin/sh
# Сторонні бібліотеки, потрібні нативному рендеру чату. Збираємо під mingw-w64
# і кладемо в одну теку (/tp/include + /tp/lib), яку далі бачить build.sh.
#
# Чому закріплені коміти, а не «остання версія»: збірка через рік має дати той
# самий бінар. Оновлення сторонньої бібліотеки — окрема свідома правка цього
# файлу, а не побічний ефект перезбирання образу.
#
# Чому саме ці чотири:
#   litehtml  — розкладка й CSS. Замість Chromium: сторінка чату лишається
#               HTML+CSS, тож теми користувачів працюють далі.
#   libwebp   — 7TV і BTTV віддають емоути у WebP; WIC його не вміє.
#   nanosvg   — значки площадок і плашки Kick — це SVG; WIC його теж не вміє.
#   nlohmann  — розбір JSON, який шле Python (той самий dict, що й у чаті).
#   mbedtls   — TLS. Чат ходить лише по захищених з'єднаннях (wss://, https://),
#               а свого TLS ні в mingw, ні в нас немає. Узято 3.6 (лінія з
#               довгою підтримкою), а не 4.x: у четвірці змінили API, і
#               IXWebSocket на нього ще не розрахований.
#   ixwebsocket— WebSocket і HTTP в одному. Twitch IRC і Kick (Pusher) — це
#               саме WebSocket, а решта (значки, емоути, YouTube) — звичайні
#               запити; тягти дві бібліотеки заради цього ні до чого.
#   imgui     — рамка вікна: смужка перетягування, куточок, замок, повзунки.
#               Саме те, заради чого на C++ узагалі варто братися за інтерфейс:
#               кнопка тут — один рядок, а не клас на сто.
set -e

TP="${1:-/tp}"
# Латки до сторонніх бібліотек (див. нижче, litehtml).
SRC_PATCHES="${2:-/patches}"
mkdir -p "$TP/include" "$TP/lib"

# Потоки: Debian за замовчуванням дає mingw з win32-threads, а там немає
# std::mutex — litehtml на ньому не збирається. Беремо posix-варіант; -static
# у build.sh вкладає libwinpthread всередину, тож зайвої DLL на машині
# користувача не з'являється.
CC=x86_64-w64-mingw32-gcc-posix
CXX=x86_64-w64-mingw32-g++-posix

cat > /tmp/mingw-toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   $CC)
set(CMAKE_CXX_COMPILER $CXX)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
TC=-DCMAKE_TOOLCHAIN_FILE=/tmp/mingw-toolchain.cmake

cd /tmp

echo ">> litehtml $LITEHTML_REF"
git clone -q --depth 1 -b "$LITEHTML_REF" https://github.com/litehtml/litehtml.git
# RelWithDebInfo — це рівно «-O2 -g -DNDEBUG», і обидві частини тут навмисні.
#
# -O2, а НЕ -O3 (типовий Release у CMake): на -O3 GCC 12 під mingw збирає
# litehtml так, що розбір документа псує память — createFromString падає в
# memcpy ще до першого звернення до нашого контейнера. -fno-strict-aliasing
# не рятує, тобто річ не в аліасингу; на -O2 усе працює. Міняючи версію
# litehtml чи компілятора, перевір це заново: hominka-render-x64.exe --probe.
#
# -g: випускний бінар однаково стрипається (-s у build.sh), зате відладочний
# дає addr2line справжні «файл:рядок» і всередині litehtml.
# Наші правки до litehtml. Тримаємо їх латками поруч, а не форком: видно, що
# саме змінено й навіщо, і оновити версію — це просто перевірити, чи латки ще
# лягають (не лягли — збірка впаде голосно, а не тихо втратить правку).
#
# Порядок значущий, тому імена пронумеровані: пізніші латки спираються на
# рядки, які додали попередні (box-shadow — на opacity).
for pt in "$SRC_PATCHES"/*.patch; do
    [ -f "$pt" ] || continue
    echo "   латка: $(basename "$pt")"
    (cd litehtml && patch -p1 --forward --fuzz=3 < "$pt")
done

cmake -S litehtml -B litehtml/b $TC -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DBUILD_TESTING=OFF -DLITEHTML_BUILD_TESTING=OFF >/dev/null
cmake --build litehtml/b -j"$(nproc)" >/dev/null
cp litehtml/b/liblitehtml.a litehtml/b/src/gumbo/libgumbo.a "$TP/lib/"
cp -r litehtml/include/litehtml "$TP/include/"
cp litehtml/include/*.h "$TP/include/" 2>/dev/null || true

echo ">> libwebp $LIBWEBP_REF"
git clone -q --depth 1 -b "$LIBWEBP_REF" https://chromium.googlesource.com/webm/libwebp
cmake -S libwebp -B libwebp/b $TC -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
      -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
      -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF >/dev/null
cmake --build libwebp/b -j"$(nproc)" >/dev/null
# libwebpdemux — покадровий розбір анімованих емоутів (7TV). Потрібен разом із
# libwebp, і саме в такому порядку при лінкуванні.
cp libwebp/b/libwebp.a libwebp/b/libwebpdemux.a libwebp/b/libsharpyuv.a "$TP/lib/"
mkdir -p "$TP/include/webp"
cp libwebp/src/webp/*.h "$TP/include/webp/"

echo ">> nanosvg $NANOSVG_REF"
git clone -q https://github.com/memononen/nanosvg.git
(cd nanosvg && git checkout -q "$NANOSVG_REF")
cp nanosvg/src/nanosvg.h nanosvg/src/nanosvgrast.h "$TP/include/"

echo ">> imgui $IMGUI_REF"
git clone -q --depth 1 -b "$IMGUI_REF" https://github.com/ocornut/imgui.git
mkdir -p "$TP/include/imgui"
# Сам ImGui + рівно два бекенди: Win32 (події вікна) і DX11 (малювання). Решта
# (OpenGL, Vulkan, SDL…) нам не треба — пристрій D3D11 у нас уже свій.
cp imgui/imgui.h imgui/imgui_internal.h imgui/imconfig.h "$TP/include/imgui/"
cp imgui/imstb_*.h "$TP/include/imgui/"
cp imgui/backends/imgui_impl_win32.h imgui/backends/imgui_impl_dx11.h "$TP/include/imgui/"

# Збираємо ImGui ТУТ, а не разом із нашим кодом, і на це дві причини.
#
# Перша — попередження. Наш код збирається з -Wall -Wextra, і це правильно:
# свої помилки треба бачити. Але чужий код під ті самі мірки не писався, і
# GCC 12 видає на ImGui десяток хибних «array subscript is outside array
# bounds» (він не бачить IM_ASSERT на межі індексу). Реальні наші попередження
# тонули в цьому шумі.
#
# Друга — час. imgui.cpp великий, і перезбирати його на кожну правку нашого
# файла ні до чого: тут він компілюється раз і лягає в кеш шару.
echo "   компілюю imgui…"
IMGUI_OBJ=/tmp/imgui-obj
mkdir -p "$IMGUI_OBJ"
for f in imgui imgui_draw imgui_tables imgui_widgets; do
    $CXX -O2 -std=c++17 -w -I"$TP/include/imgui"         -c "imgui/$f.cpp" -o "$IMGUI_OBJ/$f.o"
done
for f in imgui_impl_win32 imgui_impl_dx11; do
    $CXX -O2 -std=c++17 -w -I"$TP/include/imgui"         -c "imgui/backends/$f.cpp" -o "$IMGUI_OBJ/$f.o"
done
x86_64-w64-mingw32-ar rcs "$TP/lib/libimgui.a" "$IMGUI_OBJ"/*.o
rm -rf "$IMGUI_OBJ"

# --- mbedTLS --------------------------------------------------------------
#
# Підмодулі обов'язкові: у 3.6 частина заголовків генерується з файлів, які
# лежать саме там. Без --recurse-submodules збірка падає не одразу, а на
# середині — з незрозумілою помилкою про відсутній framework.
echo ">> mbedtls $MBEDTLS_REF"
git clone -q --depth 1 --recurse-submodules -b "$MBEDTLS_REF" \
    https://github.com/Mbed-TLS/mbedtls.git
cmake -S mbedtls -B mbedtls/build $TC \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$TP" \
      -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
      -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON >/dev/null
cmake --build mbedtls/build -j"$(nproc)" >/dev/null
cmake --install mbedtls/build >/dev/null

# --- IXWebSocket ----------------------------------------------------------
# Шляхи до mbedTLS передаємо ЯВНО. Файл тулчейна mingw каже CMake шукати
# бібліотеки лише всередині свого кореня (FIND_ROOT_PATH_MODE_LIBRARY ONLY),
# а наш /tp туди не входить — тож find_package його не бачить, хоч він і
# поруч. Під Linux тулчейна немає, але шлях однаково не системний.
# Стиснення кадрів (permessage-deflate) вимикаємо: ані Twitch IRC, ані
# Pusher його не просять, а вмикання тягло б ще й zlib. Трафік чату —
# це текстові рядки, там нема чого стискати.
echo ">> ixwebsocket $IXWS_REF"
git clone -q --depth 1 -b "$IXWS_REF" https://github.com/machinezone/IXWebSocket.git ixws
cmake -S ixws -B ixws/build $TC \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$TP" \
      -DCMAKE_PREFIX_PATH="$TP" \
      -DUSE_TLS=ON -DUSE_MBED_TLS=ON -DUSE_ZLIB=OFF \
      -DMBEDTLS_INCLUDE_DIRS="$TP/include" \
      -DMBEDTLS_LIBRARY="$TP/lib/libmbedtls.a" \
      -DMBEDX509_LIBRARY="$TP/lib/libmbedx509.a" \
      -DMBEDCRYPTO_LIBRARY="$TP/lib/libmbedcrypto.a" \
      -DIXWEBSOCKET_INSTALL=ON >/dev/null
cmake --build ixws/build -j"$(nproc)" >/dev/null
cmake --install ixws/build >/dev/null

echo ">> nlohmann/json $JSON_REF"
git clone -q --depth 1 -b "$JSON_REF" https://github.com/nlohmann/json.git
mkdir -p "$TP/include/nlohmann"
cp json/single_include/nlohmann/json.hpp "$TP/include/nlohmann/"

rm -rf /tmp/litehtml /tmp/libwebp /tmp/nanosvg /tmp/json /tmp/imgui
echo ">> сторонні бібліотеки готові в $TP"
ls -la "$TP/lib"
