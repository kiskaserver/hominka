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
# Свої включення — від native/ і native/render/include, тож у кожному рядку
# «#include» видно теку, з якої річ.
INC="-isystem $TPL/include -isystem $TPL/include/imgui -isystem $TPL/include/SDL2 -I $SRC -I $SRC/render/include $(pkg-config --cflags freetype2 fontconfig)"
LIBS="-L$TPL/lib -limgui -lSDL2 -lGL -lmonocypher -llitehtml -lgumbo -lblend2d -lwebpdemux -lwebp -lsharpyuv
      -lixwebsocket -lmbedtls -lmbedx509 -lmbedcrypto
      $(pkg-config --libs freetype2 fontconfig x11 xext) -lpthread -lrt -lm -ldl"

COMMON="-O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter"

# Той самий поділ по теках, що й під Windows. Тут менше файлів: вікна
# налаштувань і редактора теми на Linux поки немає (ImGui-бекенда для X11 у
# нас теж), а оновлення їде AppImage'ом.
SRCS="
    $SRC/render/src/app/main_linux.cpp
    $SRC/render/src/app/nettest.cpp

    $SRC/render/src/core/chat_doc.cpp
    $SRC/render/src/core/config.cpp
    $SRC/render/src/core/feed.cpp
    $SRC/render/src/core/feedgfx.cpp

    $SRC/render/src/gfx/container_bl.cpp
    $SRC/render/src/gfx/cssbits.cpp
    $SRC/render/src/gfx/fontstore.cpp
    $SRC/render/src/gfx/imgcache.cpp

    $SRC/render/src/net/badges.cpp
    $SRC/render/src/net/chatnet.cpp
    $SRC/render/src/net/imgfetch.cpp
    $SRC/render/src/net/emotes.cpp
    $SRC/render/src/net/net_http.cpp
    $SRC/render/src/net/src_kick.cpp
    $SRC/render/src/net/src_site.cpp
    $SRC/render/src/net/src_twitch.cpp
    $SRC/render/src/net/src_youtube.cpp
    $SRC/render/src/net/viewers.cpp

    $SRC/render/src/update/release.cpp
    $SRC/render/src/update/update_view.cpp
    $SRC/render/src/update/updater_unix.cpp

    $SRC/render/src/platform/ipc.cpp
    $SRC/render/src/platform/ipc_unix.cpp
    $SRC/render/src/platform/x11_window.cpp

    $SRC/render/src/ui/chrome_bl.cpp
    $SRC/render/src/ui/cssedit_ui.cpp
    $SRC/render/src/ui/csslint.cpp
    $SRC/render/src/ui/cssref.cpp
    $SRC/render/src/ui/gui_win_sdl.cpp
    $SRC/render/src/ui/samples.cpp
    $SRC/render/src/ui/settings_ui.cpp
    $SRC/render/src/ui/uibits.cpp
    $SRC/render/src/ui/uifont.cpp
"

echo ">> linux: hominka-render (нативний рендер чату)"
g++ $COMMON $INC $SRCS -o "$OUT/hominka-render-linux" $LIBS

echo ""
echo "Готово. У $OUT:"
ls -la "$OUT"
