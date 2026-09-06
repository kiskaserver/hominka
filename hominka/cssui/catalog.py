"""Що можна стилізувати і готові приклади.

Це не «документація десь у файлі», а те, що людина бачить у вікні: інакше
писати CSS для чужої розмітки — гра в вгадайку.
"""

# --- що взагалі можна стилізувати ------------------------------------------
#
# Це не «документація десь у файлі», а те, що людина бачить у вікні: інакше
# писати CSS для чужої розмітки — гра в вгадайку.
SELECTORS = [
    ("#list", "Уся стрічка",
     "Колонка з повідомленнями: відступи від країв, напрямок і проміжок між рядками.",
     "#list {\n  inset: 16px;      /* відступи від країв вікна */\n  gap: 10px;        /* проміжок між рядками */\n}"),
    (".m", "Одне повідомлення",
     "Обгортка кожного рядка: фон, рамка, скруглення, поля.",
     ".m {\n  background: rgba(0,0,0,.45);\n  padding: 3px 8px;\n  border-radius: 10px;\n}"),
    ('.m[data-platform="twitch"]', "Тільки з Twitch",
     "Те саме, але лише для Twitch. Так само: kick, youtube, site (чат сайту).",
     '.m[data-platform="twitch"] {\n  border-left: 3px solid #9146ff;\n  padding-left: 6px;\n}'),
    ('.m[data-platform="kick"]', "Тільки з Kick",
     "Повідомлення, що прийшли з Kick.",
     '.m[data-platform="kick"] {\n  border-left: 3px solid #53fc18;\n  padding-left: 6px;\n}'),
    ('.m[data-platform="youtube"]', "Тільки з YouTube",
     "Повідомлення, що прийшли з YouTube.",
     '.m[data-platform="youtube"] {\n  border-left: 3px solid #ff0033;\n  padding-left: 6px;\n}'),
    ('.m[data-kind="money"]', "Донат / Super Chat",
     "Рядок із грошима. Поряд працює клас .paid — типове оформлення донату.",
     '.m[data-kind="money"] {\n  background: rgba(251,191,36,.28);\n  border-radius: 10px;\n}'),
    ('.m[data-kind="system"]', "Будь-яка подія площадки",
     "Рейди, підписки, оголошення — усе разом. Нижче кожна окремо.",
     '.m[data-kind="system"] {\n  opacity: .7;\n}'),
    ('.m[data-event="raid"]', "Рейд",
     "До вас привели глядачів. Окремий вигляд — щоб не проґавити.",
     '.m[data-event="raid"] {\n  background: rgba(168,85,247,.35);\n'
     '  border-left: 4px solid #a855f7;\n  padding: 3px 8px;\n}'),
    ('.m[data-event="sub"]', "Підписка",
     "Нова підписка або поновлення. Спонсорство YouTube — теж сюди.",
     '.m[data-event="sub"] {\n  background: rgba(34,197,94,.28);\n'
     '  border-radius: 8px;\n  padding: 2px 8px;\n}'),
    ('.m[data-event="gift"]', "Подарована підписка",
     "Хтось подарував підписку іншим — привід окремий від власної підписки.",
     '.m[data-event="gift"] {\n  background: rgba(236,72,153,.28);\n'
     '  border-radius: 8px;\n  padding: 2px 8px;\n}'),
    ('.m[data-event="announce"]', "Оголошення",
     "Оголошення ведучого чи модератора (Twitch /announce).",
     '.m[data-event="announce"] {\n  border-left: 4px solid #38bdf8;\n  padding-left: 8px;\n}'),
    ('.m[data-event="pin"]', "Закріплене",
     "Повідомлення, закріплене в чаті площадки.",
     '.m[data-event="pin"] {\n  outline: 1px solid #fbbf24;\n  border-radius: 8px;\n}'),
    ('.m[data-event="points"]', "Бали каналу",
     "Витрачені бали каналу (Kick, Twitch-нагороди).",
     '.m[data-event="points"] {\n  opacity: .6;\n}'),
    ('.m[data-event="bits"]', "Біти Twitch",
     "Повідомлення з бітами. Поряд працює .money — сама плашка з сумою.",
     '.m[data-event="bits"] {\n  background: rgba(145,70,255,.25);\n}'),
    ('.m[data-event="superchat"]', "Super Chat",
     "Платне повідомлення YouTube.",
     '.m[data-event="superchat"] {\n  background: rgba(255,0,51,.22);\n}'),
    ('.m[data-event="mode"]', "Зміна режиму чату",
     "«Тільки для підписників» і подібні службові рядки.",
     '.m[data-event="mode"] {\n  display: none;\n}'),
    (".ico", "Значок площадки",
     "Логотип Twitch / Kick / YouTube перед рядком. Розмір в em — тягнеться за текстом.",
     ".ico {\n  width: 1.2em;\n  height: 1.2em;\n}"),
    (".b", "Значок автора",
     "MOD, VIP, SUB, HOST і решта текстових плашок біля ніка.",
     ".b {\n  border-radius: 999px;\n  font-size: .5em;\n}"),
    (".bi", "Іконка значка",
     "Справжня картинка значка (Twitch/Kick/YouTube) на місці плашки, коли вона є.",
     ".bi {\n  height: 1.4em;\n}"),
    (".n", "Нік автора",
     "Колір приходить із площадки і стоїть інлайном — свій треба ставити з !important.",
     ".n {\n  color: #ffd166 !important;\n  font-weight: 800;\n}"),
    (".money", "Сума донату",
     "Жовта плашка з сумою.",
     ".money {\n  background: #22c55e;\n  color: #052e16;\n}"),
    (".re", "Кому відповідають",
     "Рядок «↳ нік» перед текстом відповіді.",
     ".re {\n  color: #c4b5fd;\n  font-size: .75em;\n}"),
    (".t", "Текст повідомлення",
     "Сам текст (без ніка й плашок). Раніше стилізувати його окремо було нічим.",
     ".t {\n  font-weight: 500;\n  color: #f4f4f5;\n}"),
    (".n::after", "Двокрапка після ніка",
     "Малюється стилем, а не текстом — тому її можна прибрати або замінити.",
     ".n::after {\n  content: '';\n}"),
    (".em", "Емоут",
     "Картинка емоута всередині тексту.",
     ".em {\n  height: 2em;\n}"),
    (".at", "Звертання @нік",
     "Підсвічене звертання в тексті повідомлення.",
     ".at {\n  background: #f59e0b;\n  color: #111;\n}"),
    (".sys", "Текст системного",
     "Курсив рейдів, підписок і подібного.",
     ".sys {\n  font-style: normal;\n  color: #c4b5fd;\n}"),
    (".paid", "Оформлення донату",
     "Жовта смуга ліворуч і підкладка для повідомлень із грошима.",
     ".paid {\n  border-left-color: #22c55e;\n}"),
    ("body", "Загальні налаштування",
     "Шрифт, базовий кегль, колір тексту, тінь під текстом.",
     "body {\n  font-size: 26px;\n  text-shadow: 0 0 4px #000, 0 2px 3px #000;\n}"),
    ("@keyframes in", "Поява рядка",
     "Анімація, з якою новий рядок виїжджає знизу. Можна замінити своєю.",
     "@keyframes in {\n  from { opacity: 0; transform: translateX(-12px); }\n}"),
]


# --- готові рецепти ---------------------------------------------------------
#
# Найчастіше людині потрібно не «дізнатися про клас», а зробити одну конкретну
# річ: збільшити текст, прибрати плашки, підсвітити донати. Тому поруч із
# довідником — список готових шматків: подвійний клік вставляє, наведення
# показує, що саме вставиться.
RECIPES = [
    ("Крупніший текст", "Найчастіша правка: чат у кадрі дрібний.",
     "body { font-size: 26px; }"),
    ("Компактні рядки", "Більше повідомлень в тій самій висоті.",
     "#list { gap: 2px; }\n.m { line-height: 1.15; }"),
    ("Підкладка під рядком", "Читається на будь-якій картинці, не лише на темній.",
     ".m {\n  background: rgba(0,0,0,.5);\n  padding: 3px 8px;\n  border-radius: 10px;\n}"),
    ("Смуга кольору площадки", "Видно з одного погляду, звідки прийшло повідомлення.",
     '.m[data-platform="twitch"] { border-left: 3px solid #9146ff; padding-left: 6px; }\n'
     '.m[data-platform="kick"]   { border-left: 3px solid #53fc18; padding-left: 6px; }\n'
     '.m[data-platform="youtube"]{ border-left: 3px solid #ff0033; padding-left: 6px; }'),
    ("Прибрати значки автора", "MOD/VIP/SUB зникають, лишається нік.",
     ".b { display: none; }"),
    ("Прибрати іконки площадок", "Коли площадка одна, значок лише займає місце.",
     ".ico { display: none; }"),
    ("Свій колір ніків", "Площадка ставить свій колір інлайном — тому !important.",
     ".n { color: #ffd166 !important; font-weight: 800; }"),
    ("Донати помітніше", "Гроші не мають губитися серед звичайних рядків.",
     '.m[data-kind="money"] {\n  background: rgba(251,191,36,.30);\n  border-radius: 10px;\n}\n'
     ".money { font-size: .9em; }"),
    ("Системні тихіше", "Рейди й підписки не перебивають розмову.",
     ".sys { opacity: .55; font-size: .78em; }"),
    ("Без анімації появи", "Якщо рух у кадрі відволікає.",
     ".m { animation: none; }"),
    ("Товстіший контур тексту", "Читається навіть на світлій грі.",
     "body { text-shadow: 0 0 4px #000, 0 0 8px #000, 0 2px 3px #000; }"),
    ("Більші емоути", "Емоути на всю висоту рядка.",
     ".em { height: 2em; }"),
    ("Сховати чат сайту", "Лишити тільки площадки.",
     '.m[data-platform="site"] { display: none; }'),
    ("Яскравіші звертання", "Коли звертаються до вас — має кидатися в очі.",
     ".at { background: #f59e0b; color: #111; border-radius: .3em; }"),
    ("Рядок вліво, а не знизу", "Інша анімація появи.",
     "@keyframes in { from { opacity: 0; transform: translateX(-14px); } }"),
    ("Все праворуч", "Чат притиснутий до правого краю вікна.",
     "#list { align-items: flex-end; text-align: right; }"),
    ("Рейд і підписки окремими кольорами", "Щоб не загубити подію в потоці розмови.",
     '.m[data-event="raid"] { background: rgba(168,85,247,.35); padding: 3px 8px;\n'
     '                        border-radius: 8px; }\n'
     '.m[data-event="sub"]  { background: rgba(34,197,94,.28); padding: 2px 8px;\n'
     '                        border-radius: 8px; }\n'
     '.m[data-event="gift"] { background: rgba(236,72,153,.28); padding: 2px 8px;\n'
     '                        border-radius: 8px; }'),
    ("Без двокрапки після ніка", "Потрібне, коли нік переставили в кінець рядка.",
     ".n::after { content: ''; }"),
    ("Текст із нового рядка", "Нік зверху, повідомлення під ним.",
     ".t { display: block; }"),
    ("Сховати службові рядки", "Зміни режиму чату й витрачені бали.",
     '.m[data-event="mode"], .m[data-event="points"] { display: none; }'),
]


# --- приклади для перегляду -------------------------------------------------
#
# Навмисно різні: із значками (текстовими й іконками), з емоутами, з грошима,
# відповіді, системні події, звертання, довгі рядки. Стилі підбирають саме на
# такому наборі, а не на одному рядку. Перегляд крутить їх нескінченно, як живий
# чат: нові рядки виїжджають знизу і йдуть угору.
_EMOTE = ("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='28' height='28'>"
          "<circle cx='14' cy='14' r='13' fill='%23fbbf24'/><circle cx='9' cy='11' r='2' fill='%23111'/>"
          "<circle cx='19' cy='11' r='2' fill='%23111'/><path d='M8 18q6 5 12 0' stroke='%23111' "
          "stroke-width='2' fill='none' stroke-linecap='round'/></svg>")
_HEART = ("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='28' height='28'>"
          "<path d='M14 24C4 17 2 11 6 7q4-3 8 2 4-5 8-2c4 4 2 10-8 17z' fill='%23f472b6'/></svg>")
_FIRE = ("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='28' height='28'>"
         "<path d='M14 2c1 5-4 6-4 11a4 4 0 008 0c0-2-1-3-1-3 3 1 4 4 4 7a7 7 0 11-14 0c0-6 5-8 7-15z' "
         "fill='%23fb923c'/></svg>")

# Справжні іконки значків — той самий вбудований набір, що йде в чат (Kick),
# щоб у прикладі було видно й новий клас .bi, а не лише текстові плашки .b.
try:
    from ..badges import _KICK_BADGE_SVG as _KI
except Exception:                       # довідник має відкриватися завжди
    _KI = {}


def _icons(*ids):
    return [{"id": i, "url": _KI[i]} for i in ids if i in _KI]


SAMPLES = [
    {"platform": "twitch", "name": "GoodTheme", "nick": "goodtheme", "color": "#ff7f50",
     "badges": ["mod", "sub"], "text": "о, привіт! Kappa як воно?",
     "emotes": [{"code": "Kappa", "url": _EMOTE}]},
    # Текст без «@lazar1n» навмисно: звертання малює позначка «↳ нік», а
    # дублювати його в рядку — той самий баг, який ми прибрали в самому чаті
    # (chatsources.trim_reply_mention). Приклад мусить показувати правильне.
    {"platform": "kick", "name": "xQc_fan", "nick": "xqc_fan", "color": "#53fc18",
     "badges": ["vip"], "badgeIcons": _icons("vip"), "text": "не забудь про рейд", "reply": "lazar1n"},
    {"platform": "youtube", "name": "Оксана", "nick": "oksana", "color": "#ff6b81",
     "badges": ["member"], "text": "дякую за стрім! love", "amount": "200 UAH",
     "emotes": [{"code": "love", "url": _HEART}]},
    {"platform": "twitch", "kind": "system", "event": "raid",
     "text": "GoodTheme рейдить вас — 42 глядачі"},
    {"platform": "kick", "kind": "system", "event": "sub",
     "text": "xQc_fan підписався (3 міс.)"},
    {"platform": "youtube", "kind": "system", "event": "gift",
     "text": "Оксана подарувала 5 спонсорств"},
    {"platform": "site", "name": "lazar1n", "nick": "lazar1n", "color": "#a855f7",
     "badges": ["broadcaster"], "text": "вітаю всіх, поїхали"},
    {"platform": "twitch", "name": "Nightbot", "nick": "nightbot", "color": "#2dd4bf",
     "badges": ["mod"], "text": "!розклад — стріми пн/ср/пт о 19:00"},
    {"platform": "kick", "name": "BigDonator", "nick": "bigdonator", "color": "#fbbf24",
     "badges": ["og", "sub"], "badgeIcons": _icons("og", "sub"),
     "text": "тримай на каву fire", "amount": "500 UAH",
     "emotes": [{"code": "fire", "url": _FIRE}]},
    {"platform": "twitch", "name": "PogChampion", "nick": "pogchampion", "color": "#60a5fa",
     "text": "цей момент був неймовірний Kappa Kappa Kappa",
     "emotes": [{"code": "Kappa", "url": _EMOTE}]},
    {"platform": "youtube", "name": "Ivan", "nick": "ivan", "color": "#f87171",
     "text": "@lazar1n а коли наступний турнір?"},
    {"platform": "twitch", "kind": "system", "event": "announce",
     "text": "Оголошення: розіграш ключів за 10 хвилин!"},
    {"platform": "kick", "name": "ShyViewer", "nick": "shyviewer", "color": "#c084fc",
     "text": "перший раз на стрімі, дуже подобається"},
    {"platform": "twitch", "name": "SubGifter", "nick": "subgifter", "color": "#34d399",
     "badges": ["vip", "sub"], "text": "погнали love love", "amount": "300 bits",
     "emotes": [{"code": "love", "url": _HEART}]},
    {"platform": "youtube", "kind": "system", "event": "superchat",
     "text": "Марія — Super Chat 150 UAH: удачі на турнірі!"},
    {"platform": "site", "name": "Гість", "nick": "guest42", "color": "#94a3b8",
     "text": "а де знайти твій діскорд?"},
    {"platform": "twitch", "name": "ClipMaster", "nick": "clipmaster", "color": "#fb7185",
     "badges": ["mod"], "text": "кліп зроблено, зараз кину"},
    {"platform": "kick", "kind": "system", "event": "raid",
     "text": "SomeStreamer рейдить вас — 128 глядачів"},
    {"platform": "youtube", "name": "Олег", "nick": "oleg", "color": "#fcd34d",
     "badges": ["member"], "text": "3 місяці з тобою вже!"},
    {"platform": "twitch", "name": "HypeTrain", "nick": "hypetrain", "color": "#a78bfa",
     "text": "GG команда, красивий раунд fire Kappa",
     "emotes": [{"code": "fire", "url": _FIRE}, {"code": "Kappa", "url": _EMOTE}]},
    {"platform": "kick", "name": "LongMessage", "nick": "longmsg", "color": "#38bdf8",
     "badges": ["sub"], "text": "довге повідомлення, щоб перевірити, як стиль тримає "
     "перенос рядка й відступи, коли текст не вміщається в один рядок"},
    {"platform": "twitch", "kind": "system", "event": "sub",
     "text": "PogChampion продовжив підписку — 12 місяців поспіль!"},
    {"platform": "youtube", "name": "Katya", "nick": "katya", "color": "#f472b6",
     "text": "love стрім супер, дякую! love", "emotes": [{"code": "love", "url": _HEART}]},
    {"platform": "kick", "name": "Verified", "nick": "verified_guy", "color": "#22d3ee",
     "badges": ["verified"], "badgeIcons": _icons("verified"), "text": "офіційно підтверджую: топ"},
    {"platform": "site", "name": "lazar1n", "nick": "lazar1n", "color": "#a855f7",
     "badges": ["broadcaster"], "text": "дякую всім хто задонатив сьогодні"},
    {"platform": "twitch", "kind": "system", "event": "points",
     "text": "GoodTheme витратив бали каналу: Вибір гри"},
    {"platform": "kick", "kind": "system", "event": "pin",
     "text": "Закріплено (lazar1n): правила чату в описі"},
    {"platform": "youtube", "name": "NewMember", "nick": "newmember", "color": "#4ade80",
     "kind": "system", "event": "sub", "text": "NewMember став учасником каналу"},
    {"platform": "twitch", "name": "BitCheer", "nick": "bitcheer", "color": "#c4b5fd",
     "text": "тримай бітсів на розвиток каналу", "amount": "1000 bits"},
]

# --- перевірка синтаксису ---------------------------------------------------
