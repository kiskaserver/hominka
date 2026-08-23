"""Підсвітка звертань «@нік» у чужому чаті."""

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
