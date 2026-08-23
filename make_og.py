"""Генерує hominka-og.png — картинку посилання для сторінки /hominka.

Коли посилання на програму кидають у Telegram чи Discord, розгортається саме
ця картинка. Раніше там був банер каналу — тобто ділишся програмою, а бачать
стрім. Тут же видно, що це за програма і як вона виглядає.

Розмір 1200×630 — те, що очікують і Open Graph, і Twitter Card.

Запуск: python make_og.py   (потрібен знімок web/public/brand/hominka/chat.png)
"""

import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(os.path.dirname(HERE), "web", "public", "brand")
OUT = os.path.join(WEB, "hominka-og.png")

W, H = 1200, 630
SCALE = 2
BG_TOP = (24, 20, 30)
BG_BOT = (12, 11, 16)
VIOLET = (168, 85, 247)


def font(name: str, size: int):
    for candidate in (name, "segoeuib.ttf", "segoeui.ttf", "arialbd.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            continue
    return ImageFont.load_default()


def build() -> Image.Image:
    w, h = W * SCALE, H * SCALE
    img = Image.new("RGB", (w, h), BG_BOT)

    # тло: вертикальний градієнт + фіолетове сяйво ліворуч
    grad = Image.new("RGB", (w, h))
    px = grad.load()
    for y in range(h):
        t = y / h
        row = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = row
    img.paste(grad, (0, 0))

    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([-200 * SCALE, -160 * SCALE, 560 * SCALE, 420 * SCALE],
                                 fill=VIOLET + (90,))
    glow = glow.filter(ImageFilter.GaussianBlur(90 * SCALE))
    img.paste(Image.alpha_composite(img.convert("RGBA"), glow).convert("RGB"), (0, 0))

    draw = ImageDraw.Draw(img)

    icon_path = os.path.join(HERE, "hominka.png")
    if os.path.isfile(icon_path):
        size = 104 * SCALE
        icon = Image.open(icon_path).convert("RGBA").resize((size, size), Image.LANCZOS)
        img.paste(icon, (72 * SCALE, 86 * SCALE), icon)

    draw.text((200 * SCALE, 92 * SCALE), "HOMINKA", font=font("segoeuib.ttf", 68 * SCALE),
              fill=(242, 238, 236))
    draw.text((203 * SCALE, 168 * SCALE), "чат поверх гри, невидимий для OBS",
              font=font("segoeui.ttf", 26 * SCALE), fill=(176, 168, 182))

    lines = [
        "YouTube, Twitch і Kick однією стрічкою",
        "Глядачі оверлея не бачать — його ховає сама Windows",
        "Свій CSS, клік-крізь, оновлення з підписом",
    ]
    y = 268 * SCALE
    small = font("segoeui.ttf", 25 * SCALE)
    for line in lines:
        draw.ellipse([74 * SCALE, y + 9 * SCALE, 84 * SCALE, y + 19 * SCALE], fill=VIOLET)
        draw.text((100 * SCALE, y), line, font=small, fill=(206, 200, 210))
        y += 48 * SCALE

    draw.text((74 * SCALE, 520 * SCALE), "stream.svitix.com/hominka",
              font=font("segoeui.ttf", 24 * SCALE), fill=(140, 133, 148))

    # знімок вікна чату праворуч — щоб було видно, про що мова
    shot_path = os.path.join(WEB, "hominka", "chat.png")
    if os.path.isfile(shot_path):
        shot = Image.open(shot_path).convert("RGBA")
        target_h = 470 * SCALE
        ratio = target_h / shot.height
        shot = shot.resize((int(shot.width * ratio), target_h), Image.LANCZOS)

        # м'яка тінь під вікном
        shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        sx, sy = w - shot.width - 80 * SCALE, (h - shot.height) // 2
        ImageDraw.Draw(shadow).rectangle(
            [sx + 10 * SCALE, sy + 16 * SCALE, sx + shot.width + 10 * SCALE,
             sy + shot.height + 16 * SCALE], fill=(0, 0, 0, 170))
        shadow = shadow.filter(ImageFilter.GaussianBlur(26 * SCALE))
        img.paste(Image.alpha_composite(img.convert("RGBA"), shadow).convert("RGB"), (0, 0))
        img.paste(shot, (sx, sy), shot)

    return img.resize((W, H), Image.LANCZOS)


if __name__ == "__main__":
    build().save(OUT)
    print("готово:", OUT)
