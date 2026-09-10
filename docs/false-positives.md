# Хибні спрацювання: що надсилати в Microsoft і Google

Кожна нова версія — це новий файл із новим хешем, а отже нульова репутація.
SmartScreen і Chrome не «впізнають вірус», вони кажуть «ми цього файлу ще не
бачили». Тому цей файл не про виправдання, а про рутину: після кожного випуску
надіслати два звернення й забути.

## Що вже зроблено з нашого боку

Це не порожні слова — на це посилаються тексти нижче:

* **ресурс версії та значок** (`native/render/hominka.rc`) — «Подробиці» файлу
  заповнені: опис, версія, назва продукту, автор, копірайт;
* **маніфест програми** (`native/render/hominka.manifest`) — `asInvoker`,
  прав адміністратора програма не просить ніде;
* **жодних пакувальників.** UPX і подібне не використовуємо взагалі: у 2020-х
  сам факт UPX — це майже гарантоване спрацювання;
* **звичайна збірка** — `-O2`, статичне лінкування рантайму mingw, без
  саморозпакування й без коду, що пишеться в себе;
* **відкритий код** — github.com/kiskaserver/hominka;
* **підписані оновлення** — маніфест випуску підписаний Ed25519, програма не
  ставить непідписане (це наш власний підпис, не Authenticode).

## Чого НЕ зроблено і чому

Немає сертифіката Authenticode. Це головна причина попереджень, і чесно: без
нього репутація набирається лише завантаженнями, а кожен випуск обнуляє її
знову.

Безкоштовних сертифікатів для підпису коду не існує — на відміну від TLS, тут
немає Let's Encrypt. Але є два реальні шляхи:

* **SignPath Foundation** — безкоштовний підпис для проєктів з відкритим кодом.
  Дають сертифікат і сервіс підпису; вимагають публічний репозиторій і збірку,
  яку можна відтворити. Наш випадок під це підходить.
  <https://signpath.org/apply>
* **Azure Trusted Signing** — близько $10 на місяць, є перевірка особи для
  фізичних осіб (потрібна історія існування — зазвичай кілька років). Дешевше
  за класичні OV/EV сертифікати в рази.

EV-сертифікат дає репутацію SmartScreen одразу, але коштує сотні доларів на
рік і вимагає апаратного токена.

---

## 1. Microsoft: Defender / SmartScreen

Куди: <https://www.microsoft.com/en-us/wdsi/filesubmission> → *Software
developer* → *Incorrectly detected as malware*.

Що прикласти: `Hominka-X.Y.Z-win64.zip` цілком (усі файли з нього), і посилання
на випуск.

Поле **Additional information** — текст нижче. Замінити `X.Y.Z` і хеш.

```
Hominka is an open-source chat overlay for live streamers. It shows Twitch,
Kick and YouTube chat in a transparent always-on-top window, so the streamer
can read chat while playing full-screen games.

Source code: https://github.com/kiskaserver/hominka
Download:    https://update.svitix.com/hominka/files/Hominka-X.Y.Z-win64.zip
SHA-256:     <hash>

The archive contains:

  Hominka.exe                  the application itself (C++, no installer)
  native/injector-*.exe        launcher for the optional in-game overlay
  native/overlay-*.dll         the in-game overlay library
  native/hominka-vklayer-*.dll Vulkan layer for the same

Why this is likely flagged:

1. Every release is a freshly built, unsigned native binary with no download
   history. We do not yet have an Authenticode certificate.

2. The optional "chat inside the game" feature does load a DLL into another
   process. This is genuine DLL injection and we understand why heuristics
   react to it. It is what the feature is: to draw the chat inside a
   full-screen game's own frame, the overlay has to run in that game's
   process, the same way Discord, Steam and RTSS overlays do.

   It is off by default. The user has to turn it on in Settings, pick the game
   window themselves, and confirm a warning. It never runs unattended, never
   at startup, and never without a visible window.

3. The overlay hides itself from screen capture (WDA_EXCLUDEFROMCAPTURE) so
   the chat does not appear in the stream. This is the point of the product,
   not evasion: it is visible on the streamer's own monitor at all times.

What the program does NOT do: it does not install anything, does not write to
system directories, does not add startup entries, does not touch the registry
beyond nothing at all, does not collect or send user data anywhere. Settings
live in %LOCALAPPDATA%\Hominka\config.json. Network traffic is only to Twitch,
Kick, YouTube, the streamer's own site, and our update server
(update.svitix.com).

Updates are signed with our own Ed25519 key and verified before installation,
so a tampered update cannot be installed even if our server were compromised.

We are happy to provide anything else useful, including reproducible build
instructions — the whole build runs in Docker (native/Dockerfile).
```

---

## 2. Google: попередження Chrome про завантаження

Куди: <https://safebrowsing.google.com/safebrowsing/report_error/>

**URL-адреса, на яку хочете поскаржитися:**

```
https://update.svitix.com/hominka/files/Hominka-X.Y.Z-win64.zip
```

Якщо позначено сторінку, а не файл, — вказати ще й `https://svitix.com/hominka`
окремим зверненням.

**Додаткова інформація:**

```
This file is a release of Hominka, an open-source chat overlay for live
streamers (https://github.com/kiskaserver/hominka). It is served from
our own update server and is not bundled with anything.

The warning appears because each release is a newly built, unsigned Windows
binary with no download reputation yet, not because of anything the file does.
We are an independent project without an Authenticode certificate.

The archive contains one application (Hominka.exe) plus the libraries for an
optional feature that draws the chat inside a full-screen game. That feature
loads a library into the game process — the same technique Discord, Steam and
RivaTuner overlays use — and it is disabled by default and requires explicit
user action to enable.

The program has no installer, no bundled software, no advertising, no
telemetry. It writes only its own settings file under %LOCALAPPDATA% and talks
only to Twitch, Kick, YouTube, the streamer's own website and our update
server.

Please review and remove the warning. We can provide build reproduction steps
on request; the entire build runs in Docker from the public repository.
```

---

## 3. Якщо позначили сайт цілком

Google Search Console → властивість `svitix.com` → **Security Issues** →
*Request a review*. Той самий текст, що й вище, годиться як пояснення.

---

## Рутина після кожного випуску

1. Порахувати sha256 архіву — його друкує сам `tools/release.py`.
2. Надіслати форму Microsoft (пункт 1) із доданим архівом.
3. Якщо Chrome лається — форма Google (пункт 2).
4. Відповідь Microsoft приходить зазвичай за добу; після неї Defender
   перестає чіпати саме цю збірку. SmartScreen набирає репутацію окремо й
   повільніше — його лікує тільки сертифікат або завантаження.
