"""Повернення панелі реакцій, яку ховає наш же стиль."""

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
