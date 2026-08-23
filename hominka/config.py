"""config.json: що зберігаємо між запусками і як це читаємо.

Частина вікна (домішується в Overlay). Окремо — бо це єдине місце, де живе
формат файлу налаштувань: імена ключів, значення за замовчуванням і те, як
пережити зіпсований чи чужий файл.
"""

import json

from . import update as updater
from .paths import CONFIG_PATH
from .urls import site_chat_url
from .version import APP_VERSION


class ConfigMixin:
    """Читання і запис налаштувань. Частина Overlay."""

    def _load_config(self):
        cfg = {}
        try:
            # utf-8-sig, а не utf-8: варто відкрити config.json у «Блокноті» й
            # зберегти — Windows допише BOM, json.load на нього спіткнеться, і
            # всі налаштування мовчки скинуться на типові.
            with open(CONFIG_PATH, "r", encoding="utf-8-sig") as f:
                cfg = json.load(f)
        except Exception:
            pass
        g = cfg.get("geometry")
        if g and all(k in g for k in ("x", "y", "w", "h")):
            self.setGeometry(g["x"], g["y"], g["w"], g["h"])
        else:
            self.resize(360, 560)
            self.move(60, 60)
        op = cfg.get("opacity", 0.94)
        self.setWindowOpacity(op)
        self.zoom = float(cfg.get("zoom", 1.0))
        self.bg_alpha = float(cfg.get("bg_alpha", 0.30))
        # Чат сайту — те, що людина вписала в ⚙. Аргумент командного рядка
        # сильніший: ним відкривають чужий чат для налагодження.
        self.site_url = (cfg.get("siteChatUrl") or "").strip()
        self.panel.site_edit.setText(self.site_url)
        self.custom_css = cfg.get("customCss") or ""
        self.keep_top = bool(cfg.get("keepTop", True))
        # Дзеркало в RTSS вмикаємо не тут, а нижче — після того, як прочитаємо
        # канал оновлень. Інакше виходило, що збережене «rtss: true» оживало
        # ще до того, як програма дізнавалася, що вона в стабільному каналі,
        # де цієї можливості не існує.
        want_rtss = bool(cfg.get("rtss"))
        self._topmost.enabled = self.keep_top
        self.panel.keep_top.blockSignals(True)
        self.panel.keep_top.setChecked(self.keep_top)
        self.panel.keep_top.blockSignals(False)
        if self.feed is not None:
            self.feed.custom_css = self.custom_css
        # Ми щойно оновилися? Тоді перше, що бачить людина, — за чим саме
        # закривалося вікно. Позначку одразу гасимо: показуємо один раз.
        was = (cfg.get("updatedTo") or "").strip()
        if was and updater.parse_version(APP_VERSION) >= updater.parse_version(was):
            self._updated_banner = (APP_VERSION, cfg.get("updatedNotes") or "")
        self.updated_to = ""
        self.updated_notes = ""
        if not self._cli_url:
            self.url = site_chat_url(self.site_url)
            self.is_yt = False
        # Канал YouTube, знайдений минулого разу: id не змінюється, тож не
        # ходимо за ним щоразу. Саму трансляцію не запам'ятовуємо — вона
        # застаріває швидше, ніж програма встигає закритися.
        self.yt_channel_id = cfg.get("youtubeChannelId", "")
        self.my_channel = cfg.get("myChannel", "")
        self.panel.channel_edit.setText(self.my_channel)
        self.chat_delay = int(cfg.get("chatDelay", 0) or 0)
        self.panel.delay.blockSignals(True)
        self.panel.delay.setValue(self.chat_delay)
        self.panel.delay_val.setText("%d с" % self.chat_delay)
        self.panel.delay.blockSignals(False)
        self.twitch_channel = cfg.get("twitchChannel", "")
        self.kick_channel = cfg.get("kickChannel", "")
        self.panel.twitch_edit.setText(self.twitch_channel)
        self.panel.kick_edit.setText(self.kick_channel)
        # Оновлення: канал, з якого читаємо, і чи перевіряти самим.
        ch = cfg.get("channel", updater.DEFAULT_CHANNEL)
        self.channel = ch if any(c[0] == ch for c in updater.CHANNELS) else updater.DEFAULT_CHANNEL
        self.installed_channel = cfg.get("installedChannel", "")
        self.auto_update = bool(cfg.get("autoUpdate", True))
        idx = self.panel.channel.findData(self.channel)
        if idx >= 0:
            self.panel.channel.blockSignals(True)
            self.panel.channel.setCurrentIndex(idx)
            self.panel.channel.blockSignals(False)
        self.panel.auto_upd.blockSignals(True)
        self.panel.auto_upd.setChecked(self.auto_update)
        self.panel.auto_upd.blockSignals(False)
        self.panel.set_status("Версія %s (%s)" % (APP_VERSION, updater.channel_label(self.channel)))
        # Канал відомий — тепер вирішуємо долю експериментальних можливостей.
        # Панель будувалася зі стабільним каналом (overlay.py), тож без цього
        # рядка бета після кожного перезапуску лишалася без своїх галочок.
        if want_rtss:
            self.set_rtss(True)
        self.apply_experimental()
        self.panel.rtss.blockSignals(True)
        self.panel.rtss.setChecked(self.rtss_on)
        self.panel.rtss.blockSignals(False)
        # синхронізуємо панель з завантаженими значеннями
        self.panel.opacity.setValue(int(op * 100))
        self.panel.bg.setValue(int(self.bg_alpha * 100))
        self.panel.sync_zoom()
        self.bar.title.setText(self._title_for())
        self._apply_border(self.accent)

    def save_config(self):
        # дебаунс: реальний запис — через таймер (не на кожен resize-евент)
        self._save_timer.start()

    def _write_config(self):
        try:
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump({
                    "geometry": {"x": self.x(), "y": self.y(), "w": self.width(), "h": self.height()},
                    "opacity": round(self.windowOpacity(), 2),
                    "zoom": self.zoom,
                    "bg_alpha": round(self.bg_alpha, 2),
                    "youtubeChannelId": self.yt_channel_id,
                    "myChannel": self.my_channel,
                    "siteChatUrl": self.site_url,
                    "customCss": self.custom_css,
                    "keepTop": self.keep_top,
                    "rtss": self.rtss_on,
                    "updatedTo": self.updated_to,
                    "updatedNotes": self.updated_notes,
                    "chatDelay": self.chat_delay,
                    "twitchChannel": self.twitch_channel,
                    "kickChannel": self.kick_channel,
                    "channel": self.channel,
                    "installedChannel": self.installed_channel,
                    "autoUpdate": self.auto_update,
                }, f)
        except Exception:
            pass
