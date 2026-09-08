# Складання Hominka під Linux.
#
# Ubuntu 22.04 узято не випадково: зібране на новішій системі не запуститься на
# старішій (glibc сумісна лише вперед), а 22.04 — найстаріша, яку підтримує сам
# Qt 6.11. Так одна збірка працює і на 22.04, і на 24.04, і на свіжих Fedora.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Qt-бібліотеки потрібні САМЕ ПІД ЧАС ЗБІРКИ, а не лише в користувача:
# PyInstaller питає PySide6 про розкладку Qt, запускаючи його окремим процесом.
# Якщо PySide6 там не імпортується (бракує libglib чи libxkbfile), той процес
# мовчки падає, PyInstaller лишає один рядок warning — і кладе в архів усе,
# КРІМ плагінів платформи та ресурсів QtWebEngine. Зібране так виглядає готовим
# (навіть важить 138 МБ), а при запуску каже «Could not find the Qt platform
# plugin xcb». Тому список нижче — не «про всяк випадок», а умова правильної
# збірки.
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip python3-venv python3-dev python3-tk libpython3.10 \
        binutils zip \
        libgl1 libegl1 libxkbcommon0 libxkbcommon-x11-0 libxkbfile1 \
        libglib2.0-0 libdbus-1-3 libnss3 libnspr4 libxcomposite1 \
        libxdamage1 libxrandr2 libxtst6 libxi6 libxrender1 libxcursor1 \
        libsm6 libice6 libfontconfig1 libfreetype6 libcups2 libatspi2.0-0 \
        libdrm2 libgbm1 libcairo2 libpangocairo-1.0-0 libatk1.0-0 \
        libatk-bridge2.0-0 libxcb-cursor0 libxcb-icccm4 libxcb-image0 \
        libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 \
        libxcb-xinerama0 libasound2 \
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
COPY requirements.txt /src/requirements.txt
RUN python3 -m pip install --no-cache-dir --upgrade pip \
 && python3 -m pip install --no-cache-dir -r /src/requirements.txt pyinstaller

CMD ["/bin/bash", "/src/linux/build.sh"]
