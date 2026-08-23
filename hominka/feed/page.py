"""Сторінка стрічки: розмітка, стилі й JS, який малює повідомлення.

Чому сторінка, а не віджети Qt: чат уже показується у вбудованому браузері, і
робити другий механізм відображення заради того самого — зайва сутність. Плюс
верстка тут така сама, як в оверлеї на сайті, і виглядають вони однаково.
"""

import json

# Іконки площадок — справжні логотипи (Simple Icons), вписані прямо в сторінку:
# вона має лишатися самодостатньою, інакше залежить від мережі там, де не треба.
# Ті самі значки й у чаті на сайті (web/src/lib/platformIcons.tsx).
ICONS = {
    "twitch": ('<svg class="ico" width="16" height="16" viewBox="0 0 24 24" fill="#a970ff">'
               '<path d="M11.571 4.714h1.715v5.143H11.57zm4.715 0H18v5.143h-1.714zM6 0L1.714 '
               '4.286v15.428h5.143V24l4.286-4.286h3.428L22.286 12V0zm14.571 11.143l-3.428 '
               '3.428h-3.429l-3 3v-3H6.857V1.714h13.714Z"/></svg>'),
    "kick": ('<svg class="ico" width="16" height="16" viewBox="0 0 24 24" fill="#53fc18">'
             '<path d="M1.333 0h8v5.333H12V2.667h2.667V0h8v8H20v2.667h-2.667v2.666H20V16h2.667v8h-8'
             'v-2.667H12v-2.666H9.333V24h-8Z"/></svg>'),
    "youtube": ('<svg class="ico" width="16" height="16" viewBox="0 0 24 24" fill="#ff0033">'
                '<path d="M23.498 6.186a3.016 3.016 0 0 0-2.122-2.136C19.505 3.545 12 3.545 12 '
                '3.545s-7.505 0-9.377.505A3.017 3.017 0 0 0 .502 6.186C0 8.07 0 12 0 12s0 3.93.502 '
                '5.814a3.016 3.016 0 0 0 2.122 2.136c1.871.505 9.376.505 9.376.505s7.505 0 9.377-.505a'
                '3.015 3.015 0 0 0 2.122-2.136C24 15.93 24 12 24 12s0-3.93-.502-5.814zM9.545 15.568V8.432'
                'L15.818 12l-6.273 3.568z"/></svg>'),
}


# Позначки автора — той самий набір, що й на сайті.
BADGE_LABELS = {
    "broadcaster": ("HOST", "#ef4444", "#fff"),
    "mod": ("MOD", "#34d399", "#062"),
    "vip": ("VIP", "#e879f9", "#fff"),
    "sub": ("SUB", "#8b5cf6", "#fff"),
    "member": ("MEM", "#10b981", "#fff"),
    "verified": ("✓", "#38bdf8", "#fff"),
    "staff": ("STAFF", "#71717a", "#fff"),
    "og": ("OG", "#f59e0b", "#221"),
    "artist": ("ART", "#f472b6", "#fff"),
}


PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><style>
  html, body { margin:0; padding:0; background:transparent; overflow:hidden; }
  body {
    font: 20px/1.35 'Segoe UI', system-ui, sans-serif; color:#fff; font-weight:600;
    text-shadow: 0 2px 3px rgba(0,0,0,.95);
  }
  #list { position:fixed; inset:8px; display:flex; flex-direction:column;
          justify-content:flex-end; gap:6px; overflow:hidden; }
  .m { animation: in .18s ease-out; }
  @keyframes in { from { opacity:0; transform:translateY(6px); } }
  .ico { width:1em; height:1em; vertical-align:-0.15em; margin-right:.3em; }
  .b { display:inline-block; padding:0 .35em; border-radius:.35em; margin-right:.25em;
       font:800 .55em/1.7 'Segoe UI'; vertical-align:.15em; text-shadow:none; }
  .n { margin-right:.35em; }
  /* Двокрапка не всередині ніка, а після нього: коли нік переставляють у
     кінець рядка, «привіт усім Vasya:» виглядає безглуздо — а так її
     прибирають одним правилом .n::after { content:''; }. */
  .n::after { content: ':'; }
  .money { display:inline-block; background:#fbbf24; color:#111; text-shadow:none;
           padding:0 .4em; border-radius:.35em; margin-right:.35em; font-weight:800;
           font-size:.75em; vertical-align:.1em; }
  /* Текст повідомлення — теж елемент. Доти він був голим текстовим вузлом:
     ні стилізувати його, ні переставити не було чим. */
  .t { }
  .re { color:#a1a1aa; font-size:.8em; margin-right:.3em; }
  .em { height:1.5em; width:auto; vertical-align:-0.35em; margin:0 1px; }
  .at { background:rgba(250,204,21,.22); color:#fde68a; border-radius:.3em; padding:0 .2em; }
  .sys { color:#e9d5ff; font-style:italic; font-size:.85em; }
  .paid { background:rgba(251,191,36,.16); border-left:3px solid #fbbf24;
          padding:2px 6px; border-radius:0 8px 8px 0; }
</style>
<!-- Свій CSS користувача. Окремим тегом і НИЖЧЕ базового: так будь-яке
     правило перебиває типове без !important, а «скинути до типових» — це
     просто спорожнити цей тег. -->
<style id="userCss">__USER_CSS__</style>
</head><body><div id="list"></div>
<script>
const list = document.getElementById('list');
const MAX = 80;
const ICONS = __ICONS__, BADGES = __BADGES__;

// «@нік» — так у чаті відповідають одне одному; без підсвітки в потоці не
// видно, кому адресовано. Правило те саме, що на сайті.
const MENTION = /(?<![\\p{L}\\p{N}_@.-])@[\\p{L}\\p{N}_.-]{0,31}[\\p{L}\\p{N}_]/gu;

function esc(s) {
  return String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
}

function body(text, emotes) {
  let html = esc(text);
  // Спершу емоути (їхні коди вже екрановані так само), потім звертання.
  for (const e of emotes || []) {
    const code = esc(e.code);
    if (!code) continue;
    html = html.split(code).join('<img class="em" src="' + esc(e.url) + '" alt="' + code + '">');
  }
  return html.replace(MENTION, m => '<span class="at">' + m + '</span>');
}

function trim() { while (list.children.length > MAX) list.removeChild(list.firstChild); }

// Рядок збирається з частин, і кожна вміє намалювати себе сама. Тоді порядок
// частин — звичайний список, який можна переставити, а не намертво зашита
// послідовність плюсів.
const PARTS = {
  ico:    e => ICONS[e.platform] || '',
  // У системної події (рейд, підписка) немає ні автора, ні плашок, ні
  // відповіді — весь її текст уже в частині text. Без цієї перевірки на місці
  // ніка малювалося «undefined:».
  badges: e => e.kind === 'system' ? '' : (e.badges || []).map(b => {
            const s = BADGES[b];
            return s ? '<span class="b" style="background:' + s[1] + ';color:' + s[2] + '">'
                       + s[0] + '</span>' : '';
          }).join(''),
  reply:  e => (e.kind === 'system' || !e.reply) ? '' : '<span class="re">↳ ' + esc(e.reply) + '</span>',
  name:   e => {
            if (e.kind === 'system') return '';
            const color = /^#[0-9a-fA-F]{3,8}$/.test(e.color || '') ? e.color : '#f87171';
            return '<span class="n" style="color:' + color + '">' + esc(e.name) + '</span>';
          },
  money:  e => e.amount ? '<span class="money">' + esc(e.amount) + '</span>' : '',
  // Системні події займають те саме місце, що й текст повідомлення: місце в
  // рядку одне, а вигляд у них різний.
  text:   e => e.kind === 'system'
            ? '<span class="sys">' + esc(e.text) + '</span>'
            : '<span class="t">' + body(e.text, e.emotes) + '</span>',
};
const ORDER = ['ico', 'badges', 'reply', 'name', 'money', 'text'];
let layout = __LAYOUT__;

window.fts = {
  // Порядок задає програма (⚙ → свій CSS → «Порядок»). Невідомі імена мовчки
  // пропускаємо: чужий чи застарілий config.json не має ламати чат.
  setLayout(a) {
    const clean = (a || []).filter(id => PARTS[id]);
    layout = clean.length ? clean : ORDER.slice();
  },
  add(e) {
    const d = document.createElement('div');
    d.className = 'm' + (e.amount ? ' paid' : '');
    if (e.id) d.dataset.id = e.id;
    if (e.nick) d.dataset.nick = e.nick;
    // Площадка и вид сообщения — атрибутами: по ним пишется свой CSS
    // («.m[data-platform="twitch"]»), и это единственный способ отличить
    // Twitch от Kick, не разбирая содержимое строки.
    d.dataset.platform = e.platform || 'site';
    d.dataset.kind = e.kind === 'system' ? 'system' : (e.amount ? 'money' : 'message');
    // Що саме сталося: рейд, підписка, подарунок, біти. Без цього всі події
    // площадки виглядали однаково, і підсвітити рейд окремо від підписки не
    // було чим.
    if (e.event) d.dataset.event = e.event;
    d.innerHTML = layout.map(id => PARTS[id](e)).join('');
    list.appendChild(d);
    trim();
  },
  del(id) {
    for (const el of list.querySelectorAll('[data-id]')) {
      if (el.dataset.id === id) el.remove();
    }
  },
  purge(nick) {
    for (const el of list.querySelectorAll('[data-nick]')) {
      if (el.dataset.nick === nick) el.remove();
    }
  },
  clear() { list.innerHTML = ''; }
};
</script></body></html>
"""


# Частини рядка в тому порядку, в якому вони йшли завжди. Список тут, а не в
# налаштуваннях: це властивість самої верстки, а config.json лише каже, як їх
# переставити.
PARTS = (
    ("ico", "Значок площадки", "Логотип Twitch, Kick або YouTube."),
    ("badges", "Плашки автора", "MOD, VIP, SUB, HOST і решта."),
    ("reply", "Кому відповідають", "Рядок «↳ нік»."),
    ("name", "Нік автора", "Імʼя кольором площадки, з двокрапкою."),
    ("money", "Сума донату", "Плашка з сумою — лише в платних."),
    ("text", "Текст повідомлення", "Сам текст, емоути й системні події."),
)

DEFAULT_LAYOUT = [pid for pid, _short, _desc in PARTS]


def clean_layout(layout) -> list:
    """Порядок частин, яким можна користуватися.

    Приймаємо будь-що: config.json люди правлять руками, а набір частин
    змінюється з версіями. Невідоме викидаємо, зниклі частини дописуємо в
    кінець — так нова частина зʼявляється у всіх, а не лише в тих, хто ще не
    чіпав налаштування.

    Вимкнена частина лишається в списку з мінусом («-reply»), а не зникає з
    нього. Інакше «вимкнути» і «зникло з нової версії» — те саме, і повернути
    частину на її місце вже нікуди: вона додалася б у кінець рядка.
    """
    known = set(DEFAULT_LAYOUT)
    out, seen = [], set()
    for raw in (layout or []):
        pid = raw[1:] if isinstance(raw, str) and raw.startswith("-") else raw
        if pid in known and pid not in seen:
            out.append(raw)
            seen.add(pid)
    if not out:
        return list(DEFAULT_LAYOUT)
    out += [pid for pid in DEFAULT_LAYOUT if pid not in seen]
    return out


def page_html(custom_css: str = "", layout=None) -> str:
    """Сторінка стрічки. custom_css вставляється в окремий тег нижче базового.

    Екрануємо лише «</style»: усе інше в CSS нешкідливе, а от закритий тег
    усередині стилю вирвався б у розмітку і зробив із CSS довільний HTML.
    """
    safe = (custom_css or "").replace("</style", "<\\/style")
    return (PAGE.replace("__ICONS__", json.dumps(ICONS))
                .replace("__BADGES__", json.dumps(BADGE_LABELS))
                .replace("__LAYOUT__", json.dumps(clean_layout(layout)))
                .replace("__USER_CSS__", safe))


def apply_layout_js(layout) -> str:
    """JS, який міняє порядок частин на живій сторінці.

    Уже намальовані рядки лишаються як були: вихідної події в DOM немає, і
    перескладати нема з чого. Наступні йдуть новим порядком — за пів хвилини
    жвавого чату старих на екрані не лишається.
    """
    return "window.fts&&fts.setLayout(%s)" % json.dumps(clean_layout(layout))


def apply_css_js(custom_css: str) -> str:
    """JS, який міняє свій CSS на живій сторінці — без перезавантаження.

    Перезавантаження скинуло б усе, що вже написали в чаті, а стилі
    підбирають саме дивлячись на живі повідомлення."""
    return ("(function(c){var s=document.getElementById('userCss');"
            "if(!s){s=document.createElement('style');s.id='userCss';"
            "document.head.appendChild(s);} s.textContent=c;})(%s)"
            % json.dumps(custom_css or ""))
