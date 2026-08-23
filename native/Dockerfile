# Крос-компіляція overlay.dll та injector.exe під Windows — з Linux.
#
# Той самий підхід, що вже перевірений для Linux-збірки самої програми: беремо
# готовий mingw-w64 GCC і збираємо обидві розрядності (x86 для старих ігор, x64
# для решти). Жодного Visual Studio, жодного Windows на етапі збірки.
#
# Джерела в образ НЕ вкладаємо навмисно: build.ps1 монтує теку native як том і
# збирає завжди свіжий код. Копія в образі жила б власним життям і збирала б
# учорашнє — на цьому вже один раз згоріли.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-mingw-w64-x86-64 \
        g++-mingw-w64-i686 \
        make \
    && rm -rf /var/lib/apt/lists/*

# SRC — де лежать джерела в контейнері (том монтується сюди), $1 — куди класти.
ENV SRC=/work
WORKDIR /work
ENTRYPOINT ["/bin/sh", "/work/build.sh"]
