"""
Спільна стрічка чату: сторінка, куди зливаються повідомлення з усіх площадок.

Читачі (chat_twitch, chat_kick, chat_youtube) віддають події в спільному
вигляді, а ця сторінка їх малює. Оформлення навмисно те саме, що й у чату на
сайті: іконка площадки, значки автора, колір ніка з площадки, емоути
картинками, гроші окремою плашкою, підсвічені звертання «@нік».

Чому сторінка, а не віджети Qt: чат уже показується у вбудованому браузері, і
робити другий механізм відображення заради того самого — зайва сутність. Плюс
верстка тут така сама, як в оверлеї на сайті, і виглядають вони однаково.
"""

import json
import time

from PySide6.QtCore import QObject, QTimer, QUrl

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
  .money { display:inline-block; background:#fbbf24; color:#111; text-shadow:none;
           padding:0 .4em; border-radius:.35em; margin-right:.35em; font-weight:800;
           font-size:.75em; vertical-align:.1em; }
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

window.fts = {
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
    if (e.kind === 'system') {
      d.innerHTML = (ICONS[e.platform] || '') + '<span class="sys">' + esc(e.text) + '</span>';
    } else {
      let h = ICONS[e.platform] || '';
      for (const b of e.badges || []) {
        const s = BADGES[b];
        if (s) h += '<span class="b" style="background:' + s[1] + ';color:' + s[2] + '">' + s[0] + '</span>';
      }
      if (e.reply) h += '<span class="re">↳ ' + esc(e.reply) + '</span>';
      const color = /^#[0-9a-fA-F]{3,8}$/.test(e.color || '') ? e.color : '#f87171';
      h += '<span class="n" style="color:' + color + '">' + esc(e.name) + ':</span>';
      if (e.amount) h += '<span class="money">' + esc(e.amount) + '</span>';
      h += body(e.text, e.emotes);
      d.innerHTML = h;
    }
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


def page_html(custom_css: str = "") -> str:
    """Сторінка стрічки. custom_css вставляється в окремий тег нижче базового.

    Екрануємо лише «</style»: усе інше в CSS нешкідливе, а от закритий тег
    усередині стилю вирвався б у розмітку і зробив із CSS довільний HTML.
    """
    safe = (custom_css or "").replace("</style", "<\\/style")
    return (PAGE.replace("__ICONS__", json.dumps(ICONS))
                .replace("__BADGES__", json.dumps(BADGE_LABELS))
                .replace("__USER_CSS__", safe))


def apply_css_js(custom_css: str) -> str:
    """JS, який міняє свій CSS на живій сторінці — без перезавантаження.

    Перезавантаження скинуло б усе, що вже написали в чаті, а стилі
    підбирають саме дивлячись на живі повідомлення."""
    return ("(function(c){var s=document.getElementById('userCss');"
            "if(!s){s=document.createElement('style');s.id='userCss';"
            "document.head.appendChild(s);} s.textContent=c;})(%s)"
            % json.dumps(custom_css or ""))


# Мінімальний проміжок між рядками, коли черга розсмоктується. Саме він і
# рятує від «каші»: навіть якщо площадки віддали двадцять повідомлень за раз,
# на екран вони виходять по одному, а не стіною.
PACE_MS = 130


class ChatFeed(QObject):
    """Приймає події читачів і малює їх на сторінці у вікні чату.

    Уміє тримати повідомлення на затримці. Затримка потрібна з двох причин:
    підігнати чат під затримку самої трансляції і — головне — встигати читати,
    коли пишуть швидше, ніж людина встигає дивитися.
    """

    def __init__(self, view, parent=None):
        super().__init__(parent)
        self.view = view
        self.custom_css = ""
        self.ready = False
        self._queue = []          # чекають завантаження сторінки
        self._delayed = []        # (коли показати, подія)
        self.delay = 0.0
        self._timer = QTimer(self)
        self._timer.setInterval(PACE_MS)
        self._timer.timeout.connect(self._flush)

    def set_delay(self, seconds: float):
        self.delay = max(0.0, float(seconds or 0))
        if not self.delay:
            # Вимкнули затримку — все, що чекало, показуємо одразу, інакше
            # воно зависло б назавжди.
            pending, self._delayed = self._delayed, []
            for _due, e in pending:
                self._render(e)
            self._timer.stop()

    def load(self):
        """Показує порожню стрічку. Базова адреса потрібна, щоб браузер пускав
        картинки емоутів із чужих доменів."""
        self.ready = False
        self._queue = []
        self._delayed = []
        self.view.setHtml(page_html(self.custom_css), QUrl("https://stream.svitix.com/"))

    def set_custom_css(self, css: str):
        """Новий свій CSS — одразу на екран, не чекаючи перезавантаження."""
        self.custom_css = css or ""
        if self.ready:
            self.view.page().runJavaScript(apply_css_js(self.custom_css))

    def on_loaded(self):
        self.ready = True
        pending, self._queue = self._queue, []
        for e in pending:
            self.push(e)

    def push(self, event: dict):
        """Подія → сторінка. До завантаження складаємо в чергу, інакше перші
        повідомлення (а вони приходять одразу) просто зникли б."""
        # Від відповіді лишилося саме звертання (див. trim_reply_mention) —
        # показувати порожній рядок з ніком нема сенсу.
        if event.get("kind") == "msg" and not event.get("text") and not event.get("amount"):
            return
        if not self.ready:
            self._queue.append(event)
            if len(self._queue) > 200:
                del self._queue[:-200]
            return
        if not self.delay:
            self._render(event)
            return

        kind = event.get("kind")
        if kind in ("delete", "purge"):
            # Модератор прибрав повідомлення, яке ще навіть не показане — тоді
            # його треба не показувати зовсім, а не показати й одразу зняти.
            self._drop_pending(event)
            self._render(event)
            return
        self._delayed.append((time.monotonic() + self.delay, event))
        if not self._timer.isActive():
            self._timer.start()

    def _drop_pending(self, event: dict):
        kind, key = event.get("kind"), ""
        if kind == "delete":
            key = event.get("id", "")
            self._delayed = [(t, e) for t, e in self._delayed if e.get("id") != key or not key]
        else:
            key = event.get("nick", "")
            self._delayed = [(t, e) for t, e in self._delayed if e.get("nick") != key or not key]

    def _flush(self):
        """Випускає те, чий час настав, — по одному рядку за такт."""
        if not self._delayed:
            self._timer.stop()
            return
        due, event = self._delayed[0]
        if time.monotonic() < due:
            return
        self._delayed.pop(0)
        self._render(event)

    def _render(self, event: dict):
        kind = event.get("kind")
        if kind == "delete":
            js = "window.fts&&fts.del(%s)" % json.dumps(event.get("id", ""))
        elif kind == "purge":
            js = "window.fts&&fts.purge(%s)" % json.dumps(event.get("nick", ""))
        else:
            js = "window.fts&&fts.add(%s)" % json.dumps(event, ensure_ascii=False)
        self.view.page().runJavaScript(js)
