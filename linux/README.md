# Збірка під Linux

Збираємо в контейнері, а не на машині розробника: під Linux має значення, на
якій системі зібрано (програма не запуститься на старішій glibc, ніж та, де її
зібрали). Контейнер фіксує це раз і назавжди — Ubuntu 22.04, найстаріша з
підтриманих Qt 6.11.

```bash
docker build -t hominka-linux -f linux/Dockerfile .
docker run --rm -v "$PWD:/src" hominka-linux
```

На виході — `dist/Hominka-<версія>-linux64.zip` з одним файлом усередині.
Далі він їде в той самий випуск, що й Windows-збірка:

```bat
python release.py --version 1.9.0 --channel stable --kind minor ^
  --notes "..." --linux-zip dist\Hominka-1.9.0-linux64.zip
```

## Чому під Linux немає «невидимості для OBS»

У Windows вікно ховає від захоплення сама система (`SetWindowDisplayAffinity`
з `WDA_EXCLUDEFROMCAPTURE`). Ні X11, ні Wayland такого не вміють: жодна
програма не може заборонити себе знімати.

Але на Linux це й не потрібно так гостро — там нормально знімати не екран, а
гру:

| Спосіб у OBS | Чи потрапить оверлей у кадр |
| --- | --- |
| Window Capture (Xcomposite), X11 | ні — знімається одне вікно гри |
| Window Capture (PipeWire), Wayland | ні |
| obs-vkcapture (гра через Vulkan/OpenGL) | ні |
| Screen Capture / Display Capture | **так** — знімається весь екран разом з оверлеєм |

Тобто правило просте: знімайте вікно гри, а не екран. Програма каже про це в
консолі при старті на Linux.
