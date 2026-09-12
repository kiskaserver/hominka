#!/bin/sh
# Сторонні бібліотеки для НАТИВНОЇ Linux-збірки рендера чату.
#
# Окремо від thirdparty.sh навмисно: той збирає під mingw-w64 і кладе в /tp, цей
# збирає під сам Linux і кладе в /tpl. Тримати обидва в одному файлі означало б
# на кожному кроці питати «а це ми зараз під що?» — і рано чи пізно змішати.
#
# Що звідки береться на цьому боці:
#   litehtml  — той самий, що й під Windows, і з ТИМИ САМИМИ латками. Розкладка
#               мусить бути однакова на обох системах, інакше тема, зроблена на
#               Windows, поїде на Linux.
#   libwebp   — емоути 7TV/BTTV, зокрема анімовані.
#   blend2d   — растеризація: заливки, скруглення, градієнти, перетворення,
#               шари прозорості, масштабування картинок. Тобто рівно те, що на
#               Windows робить Direct2D.
#   FreeType  — гліфи. Береться з системи (apt): своєї збірки не тримаємо, бо
#               саме системна вміє те, заради чого він тут потрібен — кольорові
#               растрові емодзі (CBDT), а їх у чаті багато.
#   fontconfig— пошук файлу шрифту за назвою й за символом. Замінює те, що на
#               Windows робить DirectWrite сам (підбір накреслення й запасного
#               шрифту під ієрогліф чи емодзі).
#   mbedtls   — TLS. Чат ходить лише по захищених з'єднаннях (wss://, https://),
#               а свого TLS ні в mingw, ні в нас немає. Узято 3.6 (лінія з
#               довгою підтримкою), а не 4.x: у четвірці змінили API, і
#               IXWebSocket на нього ще не розрахований.
#   ixwebsocket— WebSocket і HTTP в одному. Twitch IRC і Kick (Pusher) — це
#               саме WebSocket, а решта (значки, емоути, YouTube) — звичайні
#               запити; тягти дві бібліотеки заради цього ні до чого.
#   stb_image — PNG/JPEG/GIF. Замість WIC, якого поза Windows немає. GIF він
#               віддає ВЖЕ СКЛАДЕНИМ покадрово, разом із затримками — тобто те,
#               що під Windows нам довелося робити руками.
#
# Закріплені версії — з тієї ж причини, що й у thirdparty.sh: збірка через рік
# має дати той самий бінар.
set -e

TPL="${1:-/tpl}"
SRC_PATCHES="${2:-/patches}"
mkdir -p "$TPL/include" "$TPL/lib"

: "${LITEHTML_REF:=v0.10}"
: "${LIBWEBP_REF:=v1.5.0}"
: "${BLEND2D_REF:=58ca9460b4138af6e793184682e9324e0a8053b2}"
: "${ASMJIT_REF:=4d46552dac1ae082eef4b3b6c205899556c023f7}"
: "${STB_REF:=2c980bb59875b0d32144a71867fbdebb2f77cd20}"
: "${MBEDTLS_REF:=mbedtls-3.6.7}"
: "${IXWS_REF:=v12.0.1}"
: "${SDL_REF:=release-2.30.9}"
: "${MONOCYPHER_REF:=4.0.3}"
: "${IMGUI_REF:=v1.92.9b}"

cd /tmp

# --- litehtml -------------------------------------------------------------
#
# Рівень оптимізації тут теж -O2, і з тієї самої причини, що під mingw
# (див. коментар у thirdparty.sh): на -O3 GCC 12 псує розбір документа. Під
# Linux це не перевірялося окремо, тож лишаємо перевірене значення.
echo ">> litehtml $LITEHTML_REF (linux)"
rm -rf litehtml-linux
git clone -q --depth 1 -b "$LITEHTML_REF" https://github.com/litehtml/litehtml.git litehtml-linux
# Ті самі латки й у тому самому порядку, що під Windows. Якщо тут вони не
# ляжуть — збірка впаде голосно, і це правильно: розходження розкладки між
# системами гірше за зламану збірку.
for pt in "$SRC_PATCHES"/*.patch; do
    [ -f "$pt" ] || continue
    echo "   латка: $(basename "$pt")"
    (cd litehtml-linux && patch -p1 --forward --fuzz=3 < "$pt")
done
cmake -S litehtml-linux -B litehtml-linux/build \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_TESTING=OFF -DLITEHTML_BUILD_TESTING=OFF >/dev/null
cmake --build litehtml-linux/build -j"$(nproc)" >/dev/null
cp litehtml-linux/build/liblitehtml.a "$TPL/lib/"
cp litehtml-linux/build/gumbo/libgumbo.a "$TPL/lib/" 2>/dev/null || \
    find litehtml-linux/build -name 'libgumbo.a' -exec cp {} "$TPL/lib/" \;
cp -r litehtml-linux/include/* "$TPL/include/"

# --- libwebp --------------------------------------------------------------
echo ">> libwebp $LIBWEBP_REF (linux)"
rm -rf libwebp-linux
git clone -q --depth 1 -b "$LIBWEBP_REF" https://chromium.googlesource.com/webm/libwebp libwebp-linux
cmake -S libwebp-linux -B libwebp-linux/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF -DWEBP_BUILD_ANIM_UTILS=OFF \
      -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF \
      -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
      -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF >/dev/null
cmake --build libwebp-linux/build -j"$(nproc)" >/dev/null
find libwebp-linux/build -name 'libwebp*.a' -o -name 'libsharpyuv.a' | while read -r f; do
    cp "$f" "$TPL/lib/"
done
mkdir -p "$TPL/include/webp"
cp libwebp-linux/src/webp/*.h "$TPL/include/webp/"

# --- Blend2D (+ asmjit) ---------------------------------------------------
#
# asmjit Blend2D використовує для JIT-конвеєрів растеризації: він складає код
# заливки під конкретний випадок прямо в рантаймі. Це те, що робить його
# швидким, і саме тому він тут замість «намалюю сам».
echo ">> blend2d (linux)"
rm -rf blend2d asmjit
git clone -q https://github.com/blend2d/blend2d.git blend2d
(cd blend2d && git checkout -q "$BLEND2D_REF")
git clone -q https://github.com/asmjit/asmjit.git blend2d/3rdparty/asmjit
(cd blend2d/3rdparty/asmjit && git checkout -q "$ASMJIT_REF")
cmake -S blend2d -B blend2d/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$TPL" \
      -DBLEND2D_STATIC=TRUE \
      -DBLEND2D_TEST=FALSE >/dev/null
cmake --build blend2d/build -j"$(nproc)" >/dev/null
# Розкладку заголовків Blend2D міняв між версіями, тож не копіюємо шляхи
# руками, а просимо його самого поставитися куди треба.
cmake --install blend2d/build >/dev/null
[ -f "$TPL/lib/libblend2d.a" ] || find blend2d/build -name 'libblend2d.a' \
    -exec cp {} "$TPL/lib/" \;
[ -f "$TPL/include/blend2d.h" ] || find blend2d -name 'blend2d.h' -not -path '*/build/*' \
    -exec sh -c 'cp "$1" "$2/include/"; cp -r "$(dirname "$1")/blend2d" "$2/include/"' \
    _ {} "$TPL" \;
test -f "$TPL/include/blend2d.h"

# --- stb_image ------------------------------------------------------------
echo ">> stb_image $STB_REF"
rm -rf stb
git clone -q https://github.com/nothings/stb.git stb
(cd stb && git checkout -q "$STB_REF")
cp stb/stb_image.h "$TPL/include/"
# Латка на дефект у самому stb (він там і досі, у найсвіжішому коміті).
#
# У stbi__load_gif_main кадр «два тому» — той, до якого GIF повертається за
# способом затирання 3, — рахується як out - 2 * stride. Тобто на два кадри
# ПЕРЕД початком буфера: читання чужої (часто вже звільненої) памʼяті на кожній
# такій гіфці. Знайшов Valgrind, коли з чату Twitch поїхали справжні гіфки:
# «Invalid read of size 4 у stbi__gif_load_next». Кадр два тому лежить, ясна
# річ, за адресою out + (layers - 2) * stride.
#
# Латка через sed, а не через patches/: там латки litehtml, і цикл кладе кожну
# на кожен репозиторій. Перевірка нижче зчиняє галас, якщо рядок у stb колись
# зміниться й заміна тихо не спрацює.
sed -i 's|two_back = out - 2 \* stride;|two_back = out + (layers - 2) * stride;|' \
    "$TPL/include/stb_image.h"
grep -q 'two_back = out + (layers - 2) \* stride;' "$TPL/include/stb_image.h" \
    || { echo "stb: рядок two_back не знайдено — латка не лягла"; exit 1; }

# --- mbedTLS --------------------------------------------------------------
#
# Підмодулі обов'язкові: у 3.6 частина заголовків генерується з файлів, які
# лежать саме там. Без --recurse-submodules збірка падає не одразу, а на
# середині — з незрозумілою помилкою про відсутній framework.
echo ">> mbedtls $MBEDTLS_REF"
# Обидва скрипти працюють у /tmp, і віконний міг лишити тут свої клони.
rm -rf mbedtls ixws
git clone -q --depth 1 --recurse-submodules -b "$MBEDTLS_REF" \
    https://github.com/Mbed-TLS/mbedtls.git
# Вмикаємо замки ВСЕРЕДИНІ mbedTLS — і робимо це до збірки, правкою самого
# конфігу, а не ключем компілятора.
#
# Навіщо. Ми ходимо в мережу з восьми потоків одразу: три сокети чату,
# лічильник глядачів, оновлення й чотири качальники картинок. Контексти TLS у
# кожного свої, і здавалося, що цього досить. Ні: у 3.6 увімкнено TLS 1.3, а
# його рукостискання смикає psa_crypto_init(), і це ГЛОБАЛЬНИЙ стан. Сама
# mbedTLS каже про це прямо: «у багатопотокових застосунках треба вмикати
# MBEDTLS_THREADING_C, навіть якщо контексти не спільні». Без цього два
# рукостискання, що почалися разом, псують купу — і програма падає геть в
# іншому місці, за хвилину-дві, щоразу по-новому.
#
# Чому правкою файлу, а не -DMBEDTLS_THREADING_C: прапорець змінює РОЗМІР
# структур (у них з'являються мьютекси), тож бачити його мусять усі, хто
# вмикає заголовки mbedTLS, — і сама бібліотека, і IXWebSocket. Файл бачать
# усі; ключ довелося б не забути передати в кожну збірку окремо.
sed -i 's|^//#define MBEDTLS_THREADING_C|#define MBEDTLS_THREADING_C|; s|^//#define MBEDTLS_THREADING_PTHREAD|#define MBEDTLS_THREADING_PTHREAD|'     mbedtls/include/mbedtls/mbedtls_config.h
grep -q '^#define MBEDTLS_THREADING_C' mbedtls/include/mbedtls/mbedtls_config.h     || { echo "не вийшло увімкнути MBEDTLS_THREADING_C"; exit 1; }
cmake -S mbedtls -B mbedtls/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$TPL" \
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
# Латка: закриття ОДНОГО сокета не має зносити крипто всього процесу.
#
# IXWebSocket кличе psa_crypto_init() при створенні сокета й
# mbedtls_psa_crypto_free() при його закритті. Для однієї сполуки це логічно, а
# в нас їх вісім одночасно: три сокети чату, глядачі, оновлення й чотири
# качальники картинок. PSA — стан ПРОЦЕСУ, а не сокета: качальник, який щойно
# забрав емоут і закрив сполуку, звільняв таблицю ключів, поки сусідній потік
# робив нею рукостискання. Звідси й були крахи «зіпсована купа» щоразу в
# іншому місці ntdll, за хвилину-дві після старту.
#
# Показав це Valgrind під Linux, дослівно: psa_unregister_read_under_mutex
# читає блок, звільнений із psa_wipe_all_key_slots у сусідньому потоці.
#
# Тому виклик прибираємо: PSA піднімається один раз і живе до кінця процесу —
# рівно так, як задумано для глобальної підсистеми. psa_crypto_init() лишаємо,
# він ідемпотентний, а від перегонів його береже MBEDTLS_THREADING_C вище.
sed -i 's|^\( *\)mbedtls_psa_crypto_free();|\1// прибрано латкою Hominka: глобальне крипто не звільняємо на кожен сокет|' \
    ixws/ixwebsocket/IXSocketMbedTLS.cpp
grep -q '^ *mbedtls_psa_crypto_free();' ixws/ixwebsocket/IXSocketMbedTLS.cpp \
    && { echo "ixwebsocket: mbedtls_psa_crypto_free() лишився — латка не лягла"; exit 1; }
cmake -S ixws -B ixws/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$TPL" \
      -DCMAKE_PREFIX_PATH="$TPL" \
      -DUSE_TLS=ON -DUSE_MBED_TLS=ON -DUSE_ZLIB=OFF \
      -DMBEDTLS_INCLUDE_DIRS="$TPL/include" \
      -DMBEDTLS_LIBRARY="$TPL/lib/libmbedtls.a" \
      -DMBEDX509_LIBRARY="$TPL/lib/libmbedx509.a" \
      -DMBEDCRYPTO_LIBRARY="$TPL/lib/libmbedcrypto.a" \
      -DIXWEBSOCKET_INSTALL=ON >/dev/null
cmake --build ixws/build -j"$(nproc)" >/dev/null
cmake --install ixws/build >/dev/null

# --- заголовкові: nanosvg і nlohmann/json --------------------------------
#
# Обидві — самі лише заголовки й нічого системного не знають, тож просто
# кладемо ті самі версії, що й у віконному дереві. Розходження версій між
# системами тут було б найгіршим варіантом: розбір SVG і JSON мусить бути
# однаковий.
echo ">> nanosvg + nlohmann (заголовки)"
cp /tp/include/nanosvg.h /tp/include/nanosvgrast.h "$TPL/include/" 2>/dev/null || {
    rm -rf nanosvg
    git clone -q https://github.com/memononen/nanosvg.git nanosvg
    (cd nanosvg && git checkout -q "${NANOSVG_REF:-239e102ec2c691f2902e20ace2ed36ee4a35cfe6}")
    cp nanosvg/src/nanosvg.h nanosvg/src/nanosvgrast.h "$TPL/include/"
}
mkdir -p "$TPL/include/nlohmann"
cp -r /tp/include/nlohmann/. "$TPL/include/nlohmann/" 2>/dev/null || {
    rm -rf json
    git clone -q --depth 1 -b "${JSON_REF:-v3.11.3}" https://github.com/nlohmann/json.git json
    cp -r json/single_include/nlohmann/. "$TPL/include/nlohmann/"
}

# --- monocypher -----------------------------------------------------------
#
# Перевірка підпису випуску (Ed25519) — та сама бібліотека й та сама версія, що
# під Windows. Своєї реалізації тут бути не може: підпис або перевіряється тим
# самим кодом на обох системах, або одна з них колись почне приймати те, що
# друга відкидає.
echo ">> monocypher $MONOCYPHER_REF (linux)"
rm -rf monocypher
git clone -q --depth 1 -b "$MONOCYPHER_REF"     https://github.com/LoupVaillant/Monocypher.git monocypher
cp monocypher/src/monocypher.h monocypher/src/optional/monocypher-ed25519.h "$TPL/include/"
gcc -O2 -w -I"$TPL/include" -c monocypher/src/monocypher.c -o /tmp/mc.o
gcc -O2 -w -I"$TPL/include" -c monocypher/src/optional/monocypher-ed25519.c -o /tmp/mc-ed.o
ar rcs "$TPL/lib/libmonocypher.a" /tmp/mc.o /tmp/mc-ed.o
rm -f /tmp/mc.o /tmp/mc-ed.o
rm -rf monocypher

# --- SDL2 -----------------------------------------------------------------
#
# Вікна налаштувань і редактора теми малює Dear ImGui, а йому потрібен хтось,
# хто відкриє вікно й дасть події. Офіційного бекенда під голий X11 у ImGui
# немає — є під SDL2, і саме з ним він живе найдовше.
#
# Статично й без Wayland: AppImage не має вимагати від системи нічого, чого там
# може не бути. X11 достатньо — під Wayland працює XWayland.
echo ">> SDL2 $SDL_REF (linux)"
rm -rf sdl
git clone -q --depth 1 -b "$SDL_REF" https://github.com/libsdl-org/SDL.git sdl
cmake -S sdl -B sdl/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$TPL"       -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF       -DSDL_X11=ON -DSDL_WAYLAND=OFF -DSDL_PULSEAUDIO=OFF -DSDL_ALSA=OFF       -DSDL_OSS=OFF -DSDL_JACK=OFF -DSDL_PIPEWIRE=OFF -DSDL_SNDIO=OFF       -DSDL_DISKAUDIO=OFF -DSDL_DUMMYAUDIO=OFF -DSDL_VULKAN=OFF >/dev/null
cmake --build sdl/build -j"$(nproc)" >/dev/null
cmake --install sdl/build >/dev/null
rm -rf sdl

# --- Dear ImGui -----------------------------------------------------------
#
# Ті самі файли панелей, що й під Windows: settings_ui.cpp і cssedit_ui.cpp —
# чистий ImGui й нічого віндового не знають. Різні тут лише бекенди: там Win32
# і D3D11, тут SDL2 і OpenGL3.
#
# FreeType замість вбудованого stb — з тієї самої причини, що під Windows: stb
# не виконує хінтинг, і текст виходить рваний.
echo ">> imgui $IMGUI_REF (linux)"
rm -rf imgui-linux
git clone -q --depth 1 -b "$IMGUI_REF" https://github.com/ocornut/imgui.git imgui-linux
mkdir -p "$TPL/include/imgui"
cp imgui-linux/imgui.h imgui-linux/imgui_internal.h imgui-linux/imconfig.h "$TPL/include/imgui/"
cp imgui-linux/imstb_*.h "$TPL/include/imgui/"
cp imgui-linux/backends/imgui_impl_sdl2.h imgui-linux/backends/imgui_impl_opengl3.h    imgui-linux/backends/imgui_impl_opengl3_loader.h "$TPL/include/imgui/"
cp imgui-linux/misc/freetype/imgui_freetype.h "$TPL/include/imgui/"
cat >> "$TPL/include/imgui/imconfig.h" <<'EOF'
#define IMGUI_ENABLE_FREETYPE
EOF

IMGUI_OBJ=/tmp/imgui-obj-linux
rm -rf "$IMGUI_OBJ"
mkdir -p "$IMGUI_OBJ"
IMGUI_INC="-I$TPL/include/imgui -I$TPL/include $(pkg-config --cflags freetype2)"
for f in imgui imgui_draw imgui_tables imgui_widgets; do
    g++ -O2 -std=c++17 -w $IMGUI_INC -c "imgui-linux/$f.cpp" -o "$IMGUI_OBJ/$f.o"
done
for f in imgui_impl_sdl2 imgui_impl_opengl3; do
    g++ -O2 -std=c++17 -w $IMGUI_INC -I"$TPL/include/SDL2"         -c "imgui-linux/backends/$f.cpp" -o "$IMGUI_OBJ/$f.o"
done
g++ -O2 -std=c++17 -w $IMGUI_INC -c imgui-linux/misc/freetype/imgui_freetype.cpp     -o "$IMGUI_OBJ/imgui_freetype.o"
ar rcs "$TPL/lib/libimgui.a" "$IMGUI_OBJ"/*.o
rm -rf "$IMGUI_OBJ" imgui-linux

echo ">> сторонні бібліотеки Linux готові в $TPL"
