#!/bin/sh
# Збирає нативний рендер чату під Linux.
#
# Що вже працює і що ні — коротко, щоб не шукати по коду:
#   * розкладка й малювання (litehtml + Blend2D + FreeType/fontconfig);
#   * самоперевірка «--selftest вхід.json вихід.png», тобто той самий спосіб
#     звірятися з браузером, що й під Windows.
# Вікна й каналу тут поки немає — це наступний крок.
set -e

SRC="${SRC:-/src}"
OUT="${1:-$SRC/dist}"
TPL="${TPL:-/tpl}"
mkdir -p "$OUT"

# -isystem для чужих заголовків: інакше -Wall -Wextra тонуть у попередженнях
# litehtml і Blend2D, і серед них не видно наших.
INC="-isystem $TPL/include $(pkg-config --cflags freetype2 fontconfig)"
LIBS="-L$TPL/lib -llitehtml -lgumbo -lblend2d -lwebpdemux -lwebp -lsharpyuv \
      $(pkg-config --libs freetype2 fontconfig x11 xext) -lpthread -lrt -lm -ldl"

COMMON="-O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter"

echo ">> linux: hominka-render (нативний рендер чату)"
g++ $COMMON $INC \
    "$SRC/render/main_linux.cpp" \
    "$SRC/render/container_bl.cpp" \
    "$SRC/render/fontstore.cpp" \
    "$SRC/render/cssbits.cpp" \
    "$SRC/render/chat_doc.cpp" \
    "$SRC/render/imgcache.cpp" \
    "$SRC/render/feed.cpp" \
    "$SRC/render/feedgfx.cpp" \
    "$SRC/render/ipc.cpp" \
    "$SRC/render/ipc_unix.cpp" \
    "$SRC/render/x11_window.cpp" \
    "$SRC/render/chrome_bl.cpp" \
    -o "$OUT/hominka-render-linux" \
    $LIBS

echo ""
echo "Готово. У $OUT:"
ls -la "$OUT"
