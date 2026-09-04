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
# -Wno-missing-field-initializers: ідіоматична нульова ініціалізація структур
#   (напр. VkXxxInfo x = {VK_STRUCTURE_TYPE_...}; решта полів — нулі, як і треба)
#   під -Wextra дає сотні хибних попереджень. Решту -Wextra лишаємо.
COMMON="-O2 -s -static -static-libgcc -static-libstdc++ -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"
DLL_LIBS="-ld3d11 -ld3d12 -ldxgi -ld3d9 -lopengl32 -lgdi32 -lole32 -luuid"
EXE_LIBS="-lshlwapi"

# --- Vulkan: заголовки (лише для типів; функції вантажимо в рантаймі) ---
# Складаємо ізольовану теку include, щоб -I не тягнув решту системних заголовків
# Linux у крос-компіляцію під mingw. VK_NO_PROTOTYPES у коді → жодного лінкування.
VKINC=/tmp/vkinc
mkdir -p "$VKINC"
cp -r /usr/include/vulkan "$VKINC/" 2>/dev/null || true
cp -r /usr/include/vk_video "$VKINC/" 2>/dev/null || true
DLL_INC="-I$VKINC"

# --- Шейдери оверлея → SPIR-V, вкладений C-масивом (glslang --vn) ---
# Компілюємо раз на етапі збірки; у грі байткод уже готовий.
echo ">> шейдери Vulkan → SPIR-V"
glslangValidator -V "$SRC/overlay/shaders_vk/overlay.vert" \
    -o "$SRC/overlay/vk_vert_spv.h" --vn g_vk_vert_spv
glslangValidator -V "$SRC/overlay/shaders_vk/overlay.frag" \
    -o "$SRC/overlay/vk_frag_spv.h" --vn g_vk_frag_spv

build() {
    CXX="$1"; ARCH="$2"
    echo ">> $ARCH: overlay.dll"
    $CXX $COMMON $DLL_INC -shared \
        "$SRC/overlay/dllmain.cpp" "$SRC/overlay/shaders.cpp" \
        -o "$OUT/overlay-$ARCH.dll" $DLL_LIBS -Wl,--kill-at

    echo ">> $ARCH: injector.exe"
    $CXX $COMMON -municode \
        "$SRC/injector/injector.cpp" \
        -o "$OUT/injector-$ARCH.exe" $EXE_LIBS

    # Імпліцитний шар Vulkan — окрема DLL, яку завантажувач Vulkan вставляє в
    # кожну Vulkan-гру ДО ініціалізації (лікує «пізній інжект»). Малює той самий
    # чат (overlay_vk.h). Функції Vulkan вантажить у рантаймі, тож d3d-бібліотеки
    # тут не потрібні.
    echo ">> $ARCH: hominka-vklayer.dll"
    $CXX $COMMON $DLL_INC -shared \
        "$SRC/overlay/vklayer_main.cpp" \
        -o "$OUT/hominka-vklayer-$ARCH.dll" -Wl,--kill-at
}

build x86_64-w64-mingw32-g++ x64
build i686-w64-mingw32-g++   x86

# Тестовий хост (лише x64): крихітна гра-макет на DX11, щоб було в що інжектити
# під час перевірки. У випуск не входить, тому й окремо від build().
echo ">> x64: testhost.exe (для перевірки)"
x86_64-w64-mingw32-g++ -O2 -s -static -municode -mwindows \
    "$SRC/testhost/testhost.cpp" -o "$OUT/testhost-x64.exe" -ld3d11 -ldxgi

echo ">> x64: testhost-dx12.exe (для перевірки DX12)"
x86_64-w64-mingw32-g++ -O2 -s -static -municode -mwindows \
    "$SRC/testhost/testhost_dx12.cpp" -o "$OUT/testhost-dx12-x64.exe" -ld3d12 -ldxgi

echo ">> x64: testhost-gl.exe (для перевірки OpenGL)"
x86_64-w64-mingw32-g++ -O2 -s -static -municode -mwindows \
    "$SRC/testhost/testhost_gl.cpp" -o "$OUT/testhost-gl-x64.exe" -lopengl32 -lgdi32

echo ">> x64: testhost-vk.exe (для перевірки Vulkan)"
x86_64-w64-mingw32-g++ -O2 -s -static -municode -mwindows $DLL_INC \
    "$SRC/testhost/testhost_vk.cpp" -o "$OUT/testhost-vk-x64.exe"

echo ""
echo "Готово. У $OUT:"
ls -la "$OUT"
