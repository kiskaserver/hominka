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
    ('.m[data-kind="system"]', "Системне повідомлення",
     "Рейди, підписки, оголошення самої площадки.",
     '.m[data-kind="system"] {\n  opacity: .7;\n}'),
    (".ico", "Значок площадки",
     "Логотип Twitch / Kick / YouTube перед рядком. Розмір в em — тягнеться за текстом.",
     ".ico {\n  width: 1.2em;\n  height: 1.2em;\n}"),
    (".b", "Значок автора",
     "MOD, VIP, SUB, HOST і решта плашок біля ніка.",
     ".b {\n  border-radius: 999px;\n  font-size: .5em;\n}"),
    (".n", "Нік автора",
     "Колір приходить із площадки і стоїть інлайном — свій треба ставити з !important.",
     ".n {\n  color: #ffd166 !important;\n  font-weight: 800;\n}"),
    (".money", "Сума донату",
     "Жовта плашка з сумою.",
     ".money {\n  background: #22c55e;\n  color: #052e16;\n}"),
    (".re", "Кому відповідають",
     "Рядок «↳ нік» перед текстом відповіді.",
     ".re {\n  color: #c4b5fd;\n  font-size: .75em;\n}"),
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
]


# --- приклади для перегляду -------------------------------------------------
#
# Навмисно різні: із значками, з емоутом, з грошима, відповідь, системне,
# звертання. Стилі підбирають саме на такому наборі, а не на одному рядку.
_EMOTE = ("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='28' height='28'>"
          "<circle cx='14' cy='14' r='13' fill='%23fbbf24'/><circle cx='9' cy='11' r='2' fill='%23111'/>"
          "<circle cx='19' cy='11' r='2' fill='%23111'/><path d='M8 18q6 5 12 0' stroke='%23111' "
          "stroke-width='2' fill='none' stroke-linecap='round'/></svg>")


SAMPLES = [
    {"platform": "twitch", "name": "GoodTheme", "nick": "goodtheme", "color": "#ff7f50",
     "badges": ["mod", "sub"], "text": "о, привіт! Kappa як воно?",
     "emotes": [{"code": "Kappa", "url": _EMOTE}]},
    {"platform": "kick", "name": "xQc_fan", "nick": "xqc_fan", "color": "#53fc18",
     "badges": ["vip"], "text": "@lazar1n не забудь про рейд", "reply": "lazar1n"},
    {"platform": "youtube", "name": "Оксана", "nick": "oksana", "color": "#ff6b81",
     "badges": ["member"], "text": "дякую за стрім!", "amount": "200 UAH"},
    {"platform": "twitch", "name": "raid", "kind": "system",
     "text": "GoodTheme рейдить вас — 42 глядачі"},
    {"platform": "site", "name": "lazar1n", "nick": "lazar1n", "color": "#a855f7",
     "badges": ["broadcaster"], "text": "вітаю всіх, поїхали"},
]

# --- перевірка синтаксису ---------------------------------------------------
