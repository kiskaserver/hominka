<div align="center">

<img src="assets/hominka.png" width="96" height="96" alt="Hominka">

# Hominka

**Чат твого стріму поверх гри — видно тобі, не видно OBS.**

Twitch, Kick і YouTube в одній стрічці, у твоєму власному CSS, у нативній
програмі на 6 МБ, яка не потрапляє в ефір.

[![Build](https://github.com/kiskaserver/hominka/actions/workflows/build.yml/badge.svg)](https://github.com/kiskaserver/hominka/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/kiskaserver/hominka?color=a855f7&label=release)](https://github.com/kiskaserver/hominka/releases/latest)
[![License: GPL v3](https://img.shields.io/badge/license-GPL--3.0-a855f7)](LICENSE)

[**Завантажити**](https://hominka.app/uk/) · [Що нового](CHANGELOG.md) · [Як це влаштовано](docs/architecture.md) · [Зібрати самому](docs/building.md)

[English](README.md) · Українська · [Русский](README.ru.md)

<img src="docs/images/themes.png" width="720" alt="Той самий чат у вбудованій темі і у власній CSS-темі">

</div>

## Навіщо

Коли монітор один, чат під час гри не прочитаєш, а чат, накладений поверх гри,
потрапляє в трансляцію. Вікно Hominka Windows сама виключає із захоплення
екрана: OBS знімає гру без нього — і через «Захоплення екрана», і через
«Захоплення вікна», і через «Захоплення гри», — навіть коли воно висить поверх
усього.

## Що вміє

- **Невидима для захоплення.** Це не фокус із порядком вікон: система сама
  не віддає вікно жодному способу запису екрана.
- **Одна стрічка.** Twitch, Kick і YouTube разом: значки площадок і глядачів,
  відповіді, донати, Super Chat, рейди, підписки.
- **Живі емоути.** 7TV, BetterTTV і FrankerFaceZ — анімовані, або застиглі на
  першому кадрі, або приховані, як тобі зручніше.
- **Свій CSS.** Кожне повідомлення розкладає справжній CSS-рушій. Вбудований
  редактор перевіряє тему, поки пишеш, показує правила, яких рушій не виконає,
  має готові рецепти й довідник усіх класів.
- **Не заважає.** Замкни вікно (Ctrl+Alt+Space) — і кліки йдуть просто в гру.
  Фокусу воно не бере ніколи.
- **Навіть у повноекранному.** Для ігор, що забирають екран собі, є чат
  усередині кадру гри — DirectX 9, 11, 12, OpenGL і Vulkan. В ігри з
  античитом рівня ядра Hominka не вкладається принципово.
- **Легка.** Один нативний файл, близько 50 МБ пам'яті, і жодної роботи для
  відеокарти, поки в чаті тихо. Ні браузера, ні середовища виконання.
- **Оновлюється сама — і безпечно.** Канали «стабільна», «бета» й «тестова»;
  кожен випуск підписаний Ed25519, і підпис перевіряється до встановлення.
- **Нікому нічого не передає.** Ні акаунта, ні телеметрії, ні нашого сервера
  посередині.

<div align="center">
<img src="docs/images/settings.png" width="49%" alt="Налаштування">
<img src="docs/images/css-editor.png" width="49%" alt="Редактор CSS">
</div>

## Встановлення

**Windows 10 (2004+) і 11.** Завантаж архів із [hominka.app](https://hominka.app/uk/)
або з [Releases](https://github.com/kiskaserver/hominka/releases/latest),
розпакуй куди завгодно й запусти `Hominka.exe`. Інсталятора немає;
налаштування лежать у `%LOCALAPPDATA%\Hominka`.

**Linux (x86_64, X11).** Завантаж AppImage, дозволь запуск і запусти. X11 і
Wayland не вміють ховати вікно від захоплення, тож знімай не весь екран, а
вікно гри (Window Capture, PipeWire або `obs-vkcapture`).

> SmartScreen чи Chrome можуть насторожитися на завантаження. Кожен випуск —
> новий непідписаний файл, у якого ще немає історії завантажень. Що це означає
> і що ми з цим робимо — у [docs/false-positives.md](docs/false-positives.md).

## Швидкий старт

1. Натисни шестерню у вікні чату й впиши свої канали.
2. Постав і розтягни вікно там, де зручно читати.
3. **Ctrl+Alt+Space** — і вікно замкнене: миша йде в гру.

## Збірка

Усе збирається в Docker, на будь-якій системі:

```sh
docker build -t hominka-native native                               # Windows
docker build -t hominka-linux -f linux/Dockerfile . \
  && docker run --rm -v "$PWD:/src" hominka-linux                   # Linux AppImage
```

Подробиці, тестові ігри й перевірки з командного рядка — у
[docs/building.md](docs/building.md). Документація для розробників англійською.

## Ліцензія

[GNU General Public License v3.0](LICENSE). Сторонні компоненти та їхні ліцензії —
у [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Назва — від слова *гомін*: так звучить багато голосів одразу.
