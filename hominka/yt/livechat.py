"""Перемикання «Цікавий чат» → «Чат наживо»."""

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
