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
#
# Свої включення — від кореня native/ і native/render/, тож у кожному рядку
# «#include» видно теку, з якої річ.
INC="-isystem $TPL/include -I $SRC -I $SRC/render $(pkg-config --cflags freetype2 fontconfig)"
LIBS="-L$TPL/lib -llitehtml -lgumbo -lblend2d -lwebpdemux -lwebp -lsharpyuv
      -lixwebsocket -lmbedtls -lmbedx509 -lmbedcrypto
      $(pkg-config --libs freetype2 fontconfig x11 xext) -lpthread -lrt -lm -ldl"

COMMON="-O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter"

# Той самий поділ по теках, що й під Windows. Тут менше файлів: вікна
# налаштувань і редактора теми на Linux поки немає (ImGui-бекенда для X11 у
# нас теж), а оновлення їде AppImage'ом.
SRCS="
    $SRC/render/app/main_linux.cpp
    $SRC/render/app/nettest.cpp

    $SRC/render/core/chat_doc.cpp
    $SRC/render/core/feed.cpp
    $SRC/render/core/feedgfx.cpp

    $SRC/render/gfx/container_bl.cpp
    $SRC/render/gfx/cssbits.cpp
    $SRC/render/gfx/fontstore.cpp
    $SRC/render/gfx/imgcache.cpp

    $SRC/render/net/badges.cpp
    $SRC/render/net/emotes.cpp
    $SRC/render/net/net_http.cpp
    $SRC/render/net/src_kick.cpp
    $SRC/render/net/src_site.cpp
    $SRC/render/net/src_twitch.cpp
    $SRC/render/net/src_youtube.cpp

    $SRC/render/platform/ipc.cpp
    $SRC/render/platform/ipc_unix.cpp
    $SRC/render/platform/x11_window.cpp

    $SRC/render/ui/chrome_bl.cpp
"

echo ">> linux: hominka-render (нативний рендер чату)"
g++ $COMMON $INC $SRCS -o "$OUT/hominka-render-linux" $LIBS

echo ""
echo "Готово. У $OUT:"
ls -la "$OUT"
