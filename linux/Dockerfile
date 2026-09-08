# Складання Hominka під Linux.
#
# Ubuntu 22.04 узято не випадково: зібране на новішій системі не запуститься на
# старішій — glibc сумісна лише вперед. Так одна збірка працює і на 22.04, і на
# 24.04, і на свіжих Fedora.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Python лишився в образі рівно заради одного: cmake з колеса pip (у 22.04
# власний cmake 3.22, а Blend2D вимагає 3.24). Самої програми на Python більше
# немає — ані PySide6, ані PyInstaller, ані півсотні бібліотек Qt, які раніше
# були умовою правильної збірки.
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip \
        g++ make cmake git patch ca-certificates pkg-config \
        libfreetype-dev libfontconfig1-dev libx11-dev libxext-dev \
        libgl1-mesa-dev libxcursor-dev libxrandr-dev libxi-dev libxss-dev \
        fonts-noto-core fonts-noto-color-emoji \
        file desktop-file-utils wget \
    && rm -rf /var/lib/apt/lists/*

# Нативний рендер чату збирається ТУТ, а не в образі native/: там Debian
# bookworm із glibc 2.36, а зібране на новішій glibc не запуститься на
# старішій. Уся програма орієнтується на 22.04, отже й рендер має бути
# зібраний на ній — інакше архів виглядав би готовим, а на 22.04 рендер
# мовчки не стартував би, і чат лишився б порожнім.
#
# Окремим шаром і ДО requirements: правка нашого коду не має перезбирати
# Blend2D (а це хвилини).
# CMake з 22.04 — 3.22, а Blend2D вимагає 3.24. Беремо колесо з pip: тягти
# сторонній APT-репозиторій заради однієї програми ні до чого, а версія з
# колеса та сама офіційна.
RUN python3 -m pip install --no-cache-dir "cmake>=3.28"  && cmake --version | head -1

ENV TPL=/tpl
COPY native/thirdparty_linux.sh /tmp/thirdparty_linux.sh
COPY native/patches /tmp/patches
RUN sh /tmp/thirdparty_linux.sh $TPL /tmp/patches

# appimagetool — сам AppImage; усередині контейнера FUSE немає, тож просимо
# його розпакуватися й запуститися без неї.
ENV APPIMAGE_EXTRACT_AND_RUN=1
# AppImageKit заархівовано, інструмент переїхав у AppImage/appimagetool —
# старе посилання на реліз 13 віддає 404.
RUN wget -qO /usr/local/bin/appimagetool \
      https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage \
 && chmod +x /usr/local/bin/appimagetool

WORKDIR /src

CMD ["/bin/bash", "/src/linux/build.sh"]
