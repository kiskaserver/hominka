<div align="center">

<img src="assets/hominka.png" width="96" height="96" alt="Hominka">

# Hominka

**Чат твоего стрима поверх игры — виден тебе, не виден OBS.**

Twitch, Kick, YouTube и чат твоего сайта одной лентой, в твоём собственном CSS,
в нативной программе на 6 МБ, которая не попадает в эфир.

[![Build](https://github.com/kiskaserver/hominka/actions/workflows/build.yml/badge.svg)](https://github.com/kiskaserver/hominka/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/kiskaserver/hominka?color=a855f7&label=release)](https://github.com/kiskaserver/hominka/releases/latest)
[![License: GPL v3](https://img.shields.io/badge/license-GPL--3.0-a855f7)](LICENSE)

[**Скачать**](https://hominka.app/ru/) · [Что нового](CHANGELOG.md) · [Как это устроено](docs/architecture.md) · [Собрать самому](docs/building.md)

[English](README.md) · [Українська](README.uk.md) · Русский

<img src="docs/images/themes.png" width="720" alt="Один и тот же чат во встроенной теме и в своей CSS-теме">

</div>

## Зачем

Когда монитор один, чат во время игры не прочитать, а чат, наложенный поверх
игры, попадает в трансляцию. Окно Hominka Windows сама исключает из захвата
экрана: OBS снимает игру без него — и через «Захват экрана», и через «Захват
окна», и через «Захват игры», — даже когда оно висит поверх всего.

## Что умеет

- **Невидима для захвата.** Это не фокус с порядком окон: система сама не
  отдаёт окно ни одному способу записи экрана.
- **Одна лента — четыре источника.** Twitch, Kick, YouTube и чат твоего сайта:
  значки площадок и зрителей, ответы, донаты, Super Chat, рейды, подписки.
- **Живые эмоуты.** 7TV, BetterTTV и FrankerFaceZ — анимированные, застывшие
  на первом кадре или скрытые, как тебе удобнее.
- **Свой CSS.** Каждое сообщение раскладывает настоящий CSS-движок. Встроенный
  редактор проверяет тему, пока пишешь, показывает правила, которые движок не
  выполнит, и содержит готовые рецепты и справочник всех классов.
- **Не мешает.** Замкни окно (Ctrl+Alt+Space) — и клики идут прямо в игру.
  Фокус оно не берёт никогда.
- **Даже в полноэкранном.** Для игр, забирающих экран себе, есть чат внутри
  кадра игры — DirectX 9, 11, 12, OpenGL и Vulkan. В игры с античитом уровня
  ядра Hominka не встраивается принципиально.
- **Лёгкая.** Один нативный файл, около 50 МБ памяти и никакой работы для
  видеокарты, пока в чате тихо. Ни браузера, ни среды выполнения.
- **Обновляется сама — и безопасно.** Каналы «стабильная», «бета» и «тестовая»;
  каждый выпуск подписан Ed25519, подпись проверяется до установки.
- **Никому ничего не передаёт.** Ни аккаунта, ни телеметрии, ни нашего сервера
  посередине.

<div align="center">
<img src="docs/images/settings.png" width="49%" alt="Настройки">
<img src="docs/images/css-editor.png" width="49%" alt="Редактор CSS">
</div>

## Установка

**Windows 10 (2004+) и 11.** Скачай архив с [hominka.app](https://hominka.app/ru/)
или из [Releases](https://github.com/kiskaserver/hominka/releases/latest),
распакуй куда угодно и запусти `Hominka.exe`. Установщика нет; настройки лежат
в `%LOCALAPPDATA%\Hominka`.

**Linux (x86_64, X11).** Скачай AppImage, разреши запуск и запусти. X11 и
Wayland не умеют прятать окно от захвата, так что снимай не весь экран, а окно
игры (Window Capture, PipeWire или `obs-vkcapture`).

> SmartScreen или Chrome могут насторожиться при скачивании. Каждый выпуск —
> новый неподписанный файл, у которого ещё нет истории загрузок. Что это значит
> и что мы с этим делаем — в [docs/false-positives.md](docs/false-positives.md).

## Быстрый старт

1. Нажми шестерёнку в окне чата и впиши свои каналы.
2. Поставь и растяни окно там, где удобно читать.
3. **Ctrl+Alt+Space** — и окно замкнуто: мышь идёт в игру.

## Сборка

Всё собирается в Docker, на любой системе:

```sh
docker build -t hominka-native native                               # Windows
docker build -t hominka-linux -f linux/Dockerfile . \
  && docker run --rm -v "$PWD:/src" hominka-linux                   # Linux AppImage
```

Подробности, тестовые игры и проверки из командной строки — в
[docs/building.md](docs/building.md). Документация для разработчиков на английском.

## Лицензия

[GNU General Public License v3.0](LICENSE). Сторонние компоненты и их лицензии —
в [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Название — от украинского *гомін*: так звучит множество голосов сразу.
