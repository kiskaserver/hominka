#!/bin/bash
# Той самий код під Valgrind: збірка під Linux, живий чат Twitch, вікно в Xvfb.
# Windows-частини (WIC, DComp, ImGui) тут інші, але стрічка, кеш картинок,
# декодери WebP/GIF, мережа й JSON — ті самі.
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq valgrind xvfb >/dev/null

cd /src
bash linux/build.sh >/tmp/build.log 2>&1 || { tail -30 /tmp/build.log; exit 1; }
echo "[vg] зібрано: $(ls -la dist-render/hominka-render-linux | awk '{print $5}') байт"

mkdir -p /root/.config/hominka
cat > /root/.config/hominka/config.json <<'JSON'
{
  "geometry": {"x": 40, "y": 40, "w": 430, "h": 600},
  "twitchChannel": "caedrel",
  "autoUpdate": false,
  "keepTop": true,
  "header": true,
  "animatedEmotes": "play"
}
JSON

Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:99

timeout -s INT 300 valgrind \
  --tool=memcheck --error-limit=no --num-callers=25 \
  --errors-for-leak-kinds=none --leak-check=no \
  --log-file=/src/dist-render/valgrind.txt \
  ./dist-render/hominka-render-linux --app >/tmp/app.log 2>&1 || true

echo "[vg] помилок memcheck:"
grep -c "^==" /src/dist-render/valgrind.txt || true
grep -E "Invalid (write|read|free)|Mismatched|uninitialised|Source and destination overlap" \
  /src/dist-render/valgrind.txt | sort | uniq -c | head -20
