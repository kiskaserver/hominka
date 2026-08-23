"""JS, який ми вставляємо в сторінку чату YouTube.

Чужа сторінка не наша, і кожен шматок тут — обхід її поведінки: прибрати зайве,
перемкнути «Цікавий чат» на «Чат наживо», підсвітити звертання, повернути на
місце панель реакцій.
"""

# Гарний прозорий стиль для чату YouTube (м'які рожево-фіолетові плашки).
YT_STYLE_JS = r"""
(function () {
  var id = '__cuteChatStyle';
  if (document.getElementById(id)) return;
  var s = document.createElement('style');
  s.id = id;
  s.textContent = `
    html, body, yt-live-chat-renderer, #chat, #contents, #item-list,
    #items, #item-scroller, #item-offset, #content-pages,
    tp-yt-app-drawer, #primary { background: transparent !important; }

    /* прибираємо зайвий інтерфейс YouTube.
       #action-panel НЕ ховаємо: саме туди YouTube кладе панель реакцій, а поле
       введення й так прибране своїми селекторами нижче. */
    yt-live-chat-header-renderer,
    yt-live-chat-message-input-renderer,
    yt-live-chat-ticker-renderer,
    yt-live-chat-banner-manager,
    yt-live-chat-viewer-engagement-message-renderer,
    #ticker, #panel-pages, #separator,
    #input-panel, #live-chat-message-input,
    yt-live-chat-text-message-renderer #timestamp,
    tp-yt-paper-tooltip { display: none !important; }

    /* реакції (плаваючі емодзі та їхня панель) — лишаємо видимими, лише знімаємо
       темну підкладку, щоб не було смуги на прозорому оверлеї */
    #action-panel, #reaction-control-panel-overlay,
    yt-reaction-control-panel-view-model,
    yt-emoji-fountain-view-model, #emoji-fountain {
      background: transparent !important;
    }

    /* звичайні повідомлення — м'які плашки */
    yt-live-chat-text-message-renderer {
      padding: 5px 10px !important;
      margin: 5px 7px !important;
      background: rgba(30, 22, 42, 0.42) !important;
      border-radius: 14px !important;
      box-shadow: 0 1px 8px rgba(0, 0, 0, 0.35) !important;
    }
    yt-live-chat-text-message-renderer[author-type="owner"] {
      background: rgba(236, 72, 153, 0.30) !important;
    }
    yt-live-chat-text-message-renderer[author-type="moderator"] {
      background: rgba(99, 102, 241, 0.30) !important;
    }
    yt-live-chat-text-message-renderer[author-type="member"] {
      background: rgba(16, 185, 129, 0.26) !important;
    }
    yt-live-chat-text-message-renderer #author-name {
      color: #f9a8d4 !important;
      font-weight: 800 !important;
      text-shadow: 0 1px 2px rgba(0, 0, 0, 0.6) !important;
    }
    yt-live-chat-text-message-renderer #message {
      color: #fdf2ff !important;
      font-weight: 500 !important;
      text-shadow: 0 1px 3px rgba(0, 0, 0, 0.75) !important;
    }
    /* звертання «@нік» — щоб було видно, кому відповідають (див. YT_MENTIONS_JS) */
    .__ftsMention {
      color: #fde68a !important;
      background: rgba(250, 204, 21, 0.20) !important;
      padding: 0 3px !important;
      border-radius: 6px !important;
      font-weight: 800 !important;
    }

    /* емодзі (авторські та стандартні) — не ховаємо, гарний розмір */
    yt-live-chat-text-message-renderer #message img,
    #message img.emoji, img.emoji {
      height: 1.3em !important; width: auto !important;
      vertical-align: -0.28em !important; margin: 0 1px !important;
    }
    /* Значки біля імені (спонсорство, ранг, модератор, автор каналу).
       YouTube ставить їм vertical-align: sub — буквально опускає під рядок, і
       значок висить нижче ніка.
       Підіймаємо ЛИШЕ його, зсувом: position: relative нічого не переставляє,
       тому решта рядка лишається на місці. Спроба вирівняти рядок автора
       флексом (1.3.1) значок поставила рівно, але зламала базову лінію всієї
       строки — текст повідомлення поїхав на 4.5 px вище ніка.
       Заміряно на живому чаті: значок 1.59 px нижче → 0.05, текст 0 → 0. */
    yt-live-chat-author-badge-renderer {
      position: relative !important;
      top: -0.12em !important;
    }
    yt-live-chat-author-badge-renderer img,
    yt-live-chat-author-badge-renderer #image { height: 1em !important; width: auto !important; }

    /* Super Chat / Super Sticker — лишаємо кольори YouTube, лише округлюємо */
    yt-live-chat-paid-message-renderer,
    yt-live-chat-paid-sticker-renderer {
      border-radius: 14px !important; margin: 6px 7px !important;
      overflow: hidden !important;
      box-shadow: 0 2px 12px rgba(0, 0, 0, 0.45) !important;
    }
    /* Нові учасники / етапи членства (зелений акцент).
       legacy-paid — та сама подія у старому оформленні YouTube. */
    yt-live-chat-membership-item-renderer,
    yt-live-chat-legacy-paid-message-renderer {
      border-radius: 14px !important; margin: 6px 7px !important;
      background: rgba(16, 185, 129, 0.32) !important;
      box-shadow: 0 1px 10px rgba(0, 0, 0, 0.42) !important;
    }
    yt-live-chat-membership-item-renderer #header *,
    yt-live-chat-membership-item-renderer #message {
      color: #ecfdf5 !important; text-shadow: 0 1px 2px rgba(0, 0, 0, 0.6) !important;
    }
    /* Подаровані підписки — «X подарував N підписок» / «отримав подарунок» */
    yt-live-chat-sponsorships-gift-purchase-announcement-renderer,
    yt-live-chat-sponsorships-gift-redemption-announcement-renderer {
      border-radius: 14px !important; margin: 6px 7px !important;
      background: rgba(168, 85, 247, 0.32) !important;
      box-shadow: 0 1px 10px rgba(0, 0, 0, 0.42) !important;
    }
    yt-live-chat-sponsorships-gift-purchase-announcement-renderer #content *,
    yt-live-chat-sponsorships-gift-redemption-announcement-renderer * {
      color: #faf5ff !important; text-shadow: 0 1px 2px rgba(0, 0, 0, 0.6) !important;
    }
    /* Опитування від автора каналу.
       Живе в #action-panel — тій самій смузі, де панель реакцій, тому й
       лишилося без нашого оформлення, коли ми перестали її ховати. Своє
       оформлення в опитування розраховане на СВІТЛУ тему: питання майже чорне
       (#0f0f0f), підпис сірий — на прозорому оверлеї це нечитабельно. */
    yt-live-chat-poll-renderer {
      background: rgba(30, 22, 42, 0.55) !important;
      border-radius: 14px !important;
      margin: 6px 7px !important;
      padding: 6px 8px !important;
      box-shadow: 0 1px 10px rgba(0, 0, 0, 0.42) !important;
    }
    yt-live-chat-poll-renderer #poll-question {
      color: #fdf2ff !important;
      font-weight: 700 !important;
      text-shadow: 0 1px 3px rgba(0, 0, 0, 0.75) !important;
    }
    yt-live-chat-poll-renderer yt-live-chat-poll-header-renderer yt-formatted-string,
    yt-live-chat-poll-renderer #text-container,
    yt-live-chat-poll-renderer tp-yt-paper-item {
      color: #e9d5ff !important;
    }
    /* Смужка голосів — нашим фіолетовим, а не блакитним YouTube. */
    yt-live-chat-poll-renderer #vote-percentage-bar {
      background: rgba(168, 85, 247, 0.45) !important;
      border-radius: 8px !important;
    }
    /* Відповісти все одно не вийде: голос вимагає входу, якого в програмі
       немає. Клік лише відкинув би на сторінку входу — тож не приймаємо його
       зовсім, а результати показуємо. */
    yt-live-chat-poll-renderer #endpoint,
    yt-live-chat-poll-renderer yt-live-chat-poll-choice {
      pointer-events: none !important;
      cursor: default !important;
    }

    /* Страховка на решту цієї смуги. YouTube кладе в #action-panel і те, чого
       ми ще не бачили (Q&A, промо, покупки), і робить це у світлій темі —
       текст виходить майже чорний на прозорому оверлеї. Дешевше один раз
       сказати «тут текст світлий», ніж ловити кожну нову панель очима. */
    #action-panel yt-formatted-string,
    #action-panel yt-attributed-string,
    #action-panel .yt-core-attributed-string {
      color: #f0e6ff !important;
    }

    /* Збори коштів у чаті (fundraiser) — той самий фіолетовий акцент */
    yt-live-chat-donation-announcement-renderer {
      border-radius: 14px !important; margin: 6px 7px !important;
      background: rgba(168, 85, 247, 0.28) !important;
      color: #faf5ff !important;
    }
    /* Службові повідомлення: увімкнено повільний режим, видалено модератором,
       затримано автомодерацією. Це не «зайвий інтерфейс», а те, що стрімеру
       треба бачити, тож не ховаємо — лише робимо тьмянішими за живий чат. */
    yt-live-chat-mode-change-message-renderer,
    yt-live-chat-moderation-message-renderer,
    yt-live-chat-auto-mod-message-renderer {
      margin: 4px 7px !important;
      padding: 3px 8px !important;
      border-radius: 12px !important;
      background: rgba(63, 63, 70, 0.38) !important;
      color: #e4e4e7 !important;
      font-size: 0.92em !important;
    }
    ::-webkit-scrollbar { width: 0 !important; background: transparent !important; }
  `;
  (document.head || document.documentElement).appendChild(s);
})();
"""


# Перемикання чату YouTube у режим «Чат наживо».
#
# YouTube відкриває чат у режимі «Цікавий чат» (Top chat), а він показує НЕ ВСЕ:
# ховає схожі повідомлення, повідомлення нових акаунтів і все, що вважає спамом.
# Виглядає це як «чат майже мертвий», хоча люди пишуть. Сам перемикач лежить у
# шапці чату, яку ми ховаємо стилем вище, — тобто вручну його ще й не дістати.
#
# Пункти меню завжди йдуть у порядку [Цікавий чат, Чат наживо], тож беремо
# ОСТАННІЙ — це не залежить від мови інтерфейсу. Клік по вже вибраному пункту не
# робимо, тому повтор нічого не ламає, а періодичний повтор потрібен: коли
# YouTube перезавантажує чат, режим скидається назад на «цікавий».
YT_ALL_MESSAGES_JS = r"""
(function () {
  if (window.__ftsAllMessages) return;
  window.__ftsAllMessages = true;

  function switchOnce() {
    var box = document.querySelector('#view-selector');
    if (!box) return;
    var items = box.querySelectorAll('tp-yt-paper-listbox a');
    if (items.length < 2) return;
    var all = items[items.length - 1];
    if (all.getAttribute('aria-selected') === 'true') return;  // вже все видно
    all.click();
  }

  switchOnce();
  setInterval(switchOnce, 5000);
})();
"""


# Панель реакцій YouTube.
#
# Показати її нам, найпевніше, не доведеться: YouTube віддає реакції лише тим,
# хто увійшов у акаунт (анонімному внизу чату написано «Sign in to chat»), а
# входу в програмі немає — Google не пускає у вбудований браузер.
#
# Код лишаємо: він нічого не робить, поки панелі немає, а якщо вона колись
# з'явиться (YouTube не питає нас, коли міняє правила), — переносимо її у свій
# контейнер, щоб її не приховав той самий стиль, що прибирає поле вводу.
YT_REACTIONS_JS = r"""
(function () {
  if (window.__ftsReactionsOn) return;
  window.__ftsReactionsOn = true;

  var SEL = 'yt-reaction-control-panel-view-model,' +
            'yt-live-chat-reaction-control-panel-renderer,' +
            '#reaction-control-panel-overlay';

  function host() {
    var box = document.getElementById('__ftsReactions');
    if (box) return box;
    box = document.createElement('div');
    box.id = '__ftsReactions';
    box.style.cssText = 'position:fixed;right:8px;bottom:8px;z-index:2147483000;' +
      'display:flex;gap:6px;align-items:center;background:rgba(20,16,28,0.55);' +
      'border-radius:14px;padding:2px 6px;backdrop-filter:blur(2px);';
    document.body.appendChild(box);
    return box;
  }

  function move() {
    var panel = document.querySelector(SEL);
    if (!panel) return;
    var box = host();
    if (panel.parentElement !== box) box.appendChild(panel);
    panel.style.display = 'flex';
    panel.style.visibility = 'visible';
  }

  move();
  setInterval(move, 3000);
})();
"""


# Підсвічування звертань «@нік».
#
# У чаті YouTube немає гілок відповідей: люди відповідають одне одному, пишучи
# «@vasya Привіт!». У суцільному потоці повідомлень це губиться — незрозуміло,
# кому адресовано. Тому знаходимо звертання в тексті й загортаємо у span, який
# стиль вище фарбує жовтим.
#
# Чому вручну, а не innerHTML: сторінки YouTube працюють під Trusted Types, і
# присвоєння innerHTML там кидає помилку. Тому лише createElement/appendChild.
#
# Повідомлення додаються постійно, тож слухаємо MutationObserver. Вузли YouTube
# перевикористовує під нові повідомлення, тому запам'ятовуємо текст, який уже
# розмітили: змінився текст — розмічаємо заново.
YT_MENTIONS_JS = r"""
(function () {
  if (window.__ftsMentionsOn) return;
  window.__ftsMentionsOn = true;

  // «@нік»: перед @ не має бути літери (щоб пошта a@b.com не рахувалась
  // звертанням), а закінчуватись має літерою/цифрою (щоб кома чи крапка після
  // ніка не потрапили всередину).
  var RE = /(?<![\p{L}\p{N}_@.\-])@[\p{L}\p{N}_.\-]{0,31}[\p{L}\p{N}_]/gu;

  function paint(msg) {
    var text = msg.textContent || '';
    if (msg.__ftsText === text) return;   // цей текст уже розмічено
    msg.__ftsText = text;
    if (text.indexOf('@') < 0) return;

    var walker = document.createTreeWalker(msg, NodeFilter.SHOW_TEXT);
    var nodes = [];
    while (walker.nextNode()) nodes.push(walker.currentNode);

    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i], val = node.nodeValue || '';
      if (val.indexOf('@') < 0) continue;
      if (node.parentNode && node.parentNode.className === '__ftsMention') continue;

      var frag = document.createDocumentFragment(), last = 0, m;
      RE.lastIndex = 0;
      while ((m = RE.exec(val))) {
        if (m.index > last) frag.appendChild(document.createTextNode(val.slice(last, m.index)));
        var span = document.createElement('span');
        span.className = '__ftsMention';
        span.textContent = m[0];
        frag.appendChild(span);
        last = m.index + m[0].length;
      }
      if (!last) continue;
      if (last < val.length) frag.appendChild(document.createTextNode(val.slice(last)));
      node.parentNode.replaceChild(frag, node);
    }
  }

  function scan(root) {
    if (root.nodeType !== 1) return;
    if (root.id === 'message') paint(root);
    var list = root.querySelectorAll ? root.querySelectorAll('#message') : [];
    for (var i = 0; i < list.length; i++) paint(list[i]);
  }

  scan(document.documentElement);
  new MutationObserver(function (muts) {
    for (var i = 0; i < muts.length; i++) {
      var added = muts[i].addedNodes;
      for (var j = 0; j < added.length; j++) scan(added[j]);
    }
  }).observe(document.documentElement, { childList: true, subtree: true });
})();
"""
