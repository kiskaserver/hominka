# Крос-компіляція overlay.dll та injector.exe під Windows — з Linux.
#
# Той самий підхід, що вже перевірений для Linux-збірки самої програми: беремо
# готовий mingw-w64 GCC і збираємо обидві розрядності (x86 для старих ігор, x64
# для решти). Жодного Visual Studio, жодного Windows на етапі збірки.
#
# Джерела ВКЛАДАЄМО в образ і компілюємо ПІД ЧАС його складання (RUN build.sh).
# Це навмисно, і ось чому: bind-mount теки з Windows у контейнер віддає файли з
# затримкою кешу — два docker run поспіль бачать різний вміст, і збірка тихо
# компілює вчорашній код. COPY же атомарний: шар кешу COPY зникає рівно тоді,
# коли джерело змінилося, тож у образі завжди свіжий код. Готові бінарі
# витягуємо потім через docker cp (див. build.ps1) — теж без bind-mount.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-mingw-w64-x86-64 \
        g++-mingw-w64-i686 \
        make \
        cmake \
        git \
        patch \
        ca-certificates \
        libvulkan-dev \
        glslang-tools \
        g++ \
        libfreetype-dev \
        libfontconfig1-dev \
        libx11-dev \
        libxext-dev \
        libxrender-dev \
        fonts-dejavu-core \
        fonts-noto-color-emoji \
    && rm -rf /var/lib/apt/lists/*

# Сторонні бібліотеки рендера чату — ОКРЕМИМ шаром, ДО копіювання наших джерел.
# Так правка нашого коду не перезбирає litehtml (а це хвилини), і шар лишається
# в кеші доти, доки не зміняться закріплені версії нижче.
ENV LITEHTML_REF=v0.10
ENV LIBWEBP_REF=v1.5.0
ENV NANOSVG_REF=239e102ec2c691f2902e20ace2ed36ee4a35cfe6
ENV IMGUI_REF=v1.92.9b
ENV FREETYPE_REF=VER-2-13-3
ENV JSON_REF=v3.11.3
# TLS і WebSocket: чат ходить лише по захищених з'єднаннях.
ENV MBEDTLS_REF=mbedtls-3.6.7
ENV IXWS_REF=v12.0.1
ENV MONOCYPHER_REF=4.0.3
ENV TP=/tp
COPY thirdparty.sh /tmp/thirdparty.sh
COPY patches /tmp/patches
RUN sh /tmp/thirdparty.sh $TP /tmp/patches

# Те саме, але для НАТИВНОЇ Linux-збірки рендера. Окремим шаром і після
# віконного: правка Linux-частини не має перезбирати те, що вже зібрано під
# Windows, і навпаки.
ENV TPL=/tpl
COPY thirdparty_linux.sh /tmp/thirdparty_linux.sh
RUN sh /tmp/thirdparty_linux.sh $TPL /tmp/patches

ENV SRC=/src
COPY . /src
RUN sh /src/build.sh /out
