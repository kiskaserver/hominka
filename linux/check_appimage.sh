#!/bin/bash
# Чи запускається зібраний AppImage на чистій системі.
#
# Навіщо ЧИСТА. Образ, у якому ми збирали, має все: компілятор, Qt-devel,
# шрифти, cmake. Перевіряти там означало б не помітити забуту залежність — і
# вона виявилася б уже в людини, яка просто завантажила файл.
#
# Дивимося на три речі, і жодна не зайва:
#   1. програма не впала;
#   2. вона підняла нативний рендер окремим процесом — тобто той поїхав у
#      образ і має право на виконання;
#   3. на екрані з'явилося вікно.
#
# Мережі тут немає, чат лишиться порожнім — питання не в повідомленнях, а в
# тому, чи взагалі стартує те, що ми віддаємо людині.
#
# Чекаємо ПОЯВИ вікна, а не «поспимо N секунд». Збірка одним файлом розпаковує
# себе при кожному запуску (тут це понад 200 МБ), і в контейнері без кеша
# сторінок це помітно довше, ніж на живій машині: перша спроба з паузою в 12
# секунд показала «не стартувала», хоча програма просто ще розпаковувалась.
set -uo pipefail

APP="$(ls /dist/Hominka-*-linux64.AppImage 2>/dev/null | head -1)"
if [ -z "$APP" ]; then
    echo "ПОМИЛКА: у /dist немає AppImage"
    exit 1
fi
echo "перевіряю $(basename "$APP") ($(du -h "$APP" | cut -f1))"

Xvfb :99 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:99
export HOME=/tmp/home
mkdir -p "$HOME"

# Нативний шлях умикаємо явно: без цього програма піде типовим (браузером), і
# перевірка нічого не скаже про рендер. Шлях той самий, що обирає paths.py у
# зібраній програмі поза Windows.
mkdir -p "$HOME/.config/hominka"
cat > "$HOME/.config/hominka/config.json" <<'CFG'
{"twitchChannel": "test", "autoUpdate": false,
 "geometry": {"x": 60, "y": 60, "w": 430, "h": 400}}
CFG

# Без цього Python буферизує stdout у файл, і журнал лишається порожнім навіть
# тоді, коли програма щось пише.
START=$(date +%s)
"$APP" > /tmp/app.log 2>&1 &
APP_PID=$!

WINS=0
for _ in $(seq 1 90); do
    sleep 1
    kill -0 "$APP_PID" 2>/dev/null || break
    WINS="$(xwininfo -root -tree 2>/dev/null | grep -ci 'hominka' || true)"
    [ "$WINS" -ge 1 ] && break        # вікно чату (нативна програма — одне вікно)
done
ELAPSED=$(( $(date +%s) - START ))

FAIL=0
if kill -0 "$APP_PID" 2>/dev/null; then
    echo "програма жива: так (вікна за ${ELAPSED} с)"
else
    echo "програма жива: НІ"
    FAIL=1
fi

echo "вікон Hominka на екрані: $WINS"
xwininfo -root -tree 2>/dev/null | grep -i 'hominka' | head -4 | sed 's/^/   /'
[ "$WINS" -ge 1 ] || FAIL=1

kill "$APP_PID" 2>/dev/null
wait "$APP_PID" 2>/dev/null

if [ "$FAIL" -ne 0 ]; then
    echo "--- журнал програми:"
    tail -40 /tmp/app.log
    exit 1
fi
echo ""
echo "гаразд: AppImage запускається на чистій системі й показує вікно"
