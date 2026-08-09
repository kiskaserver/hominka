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

from PySide6.QtCore import QUrl

# Іконки площадок — простими фігурами, без зовнішніх файлів: сторінка має
# лишатися самодостатньою, інакше вона залежить від мережі там, де не треба.
ICONS = {
    "twitch": ('<svg class="ico" width="16" height="16" viewBox="0 0 24 24" fill="#a970ff"><path d="M4 3h17v11l-5 5h-4l-3 3H7v-3H4V3zm2 '
               '2v10h3v3l3-3h4l3-3V5H6zm7 2h2v5h-2V7zm-5 0h2v5H8V7z"/></svg>'),
    "kick": ('<svg class="ico" width="16" height="16" viewBox="0 0 24 24" fill="#53fc18"><path d="M3 3h6v5h2V6h2V3h6v6h-2v2h-2v2h2v2h2v6h-6'
             'v-3h-2v-2H9v5H3V3z"/></svg>'),
    "youtube": ('<svg class="ico" width="16" height="16" viewBox="0 0 24 24" fill="#ff0033"><path d="M22 12s0-3.2-.4-4.7c-.2-.9-.9-1.5-1.7-1.7'
                'C18.3 5.2 12 5.2 12 5.2s-6.3 0-7.9.4c-.8.2-1.5.8-1.7 1.7C2 8.8 2 12 2 12s0 3.2.4 4.7c.2.9.9'
                ' 1.5 1.7 1.7 1.6.4 7.9.4 7.9.4s6.3 0 7.9-.4c.8-.2 1.5-.8 1.7-1.7.4-1.5.4-4.7.4-4.7zM10 15V9'
                'l5.2 3L10 15z"/></svg>'),
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
</style></head><body><div id="list"></div>
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


def page_html() -> str:
    return (PAGE.replace("__ICONS__", json.dumps(ICONS))
                .replace("__BADGES__", json.dumps(BADGE_LABELS)))


class ChatFeed:
    """Приймає події читачів і малює їх на сторінці у вікні чату."""

    def __init__(self, view):
        self.view = view
        self.ready = False
        self._queue = []

    def load(self):
        """Показує порожню стрічку. Базова адреса потрібна, щоб браузер пускав
        картинки емоутів із чужих доменів."""
        self.ready = False
        self._queue = []
        self.view.setHtml(page_html(), QUrl("https://stream.svitix.com/"))

    def on_loaded(self):
        self.ready = True
        pending, self._queue = self._queue, []
        for e in pending:
            self.push(e)

    def push(self, event: dict):
        """Подія → сторінка. До завантаження складаємо в чергу, інакше перші
        повідомлення (а вони приходять одразу) просто зникли б."""
        if not self.ready:
            self._queue.append(event)
            if len(self._queue) > 200:
                del self._queue[:-200]
            return
        kind = event.get("kind")
        if kind == "delete":
            js = "window.fts&&fts.del(%s)" % json.dumps(event.get("id", ""))
        elif kind == "purge":
            js = "window.fts&&fts.purge(%s)" % json.dumps(event.get("nick", ""))
        else:
            js = "window.fts&&fts.add(%s)" % json.dumps(event, ensure_ascii=False)
        self.view.page().runJavaScript(js)
