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
        libvulkan-dev \
        glslang-tools \
    && rm -rf /var/lib/apt/lists/*

ENV SRC=/src
COPY . /src
RUN sh /src/build.sh /out
