"""Генерує splash.png — заставку, яку видно, поки програма розпаковується.

Збірка одним файлом розкладає ~300 МБ у тимчасову теку при кожному запуску, і
до появи вікна минає кілька секунд. Заставка — єдине, що в цей час каже «воно
запускається»; під нею bootloader сам пише, що саме зараз розпаковує, а перед
самою появою вікна текст міняємо на свій.

Запуск: python make_splash.py
"""

import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 480, 240
SCALE = 3                     # малюємо більшим і зменшуємо — краї виходять гладкі
BG_TOP = (22, 18, 28)
BG_BOT = (14, 12, 18)
VIOLET = (168, 85, 247)
PINK = (236, 72, 153)


def font(name: str, size: int):
    """Системний шрифт; якщо його немає — типовий, аби не впасти на збірці."""
    for candidate in (name, "segoeuib.ttf", "segoeui.ttf", "arialbd.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            continue
    return ImageFont.load_default()


def rounded(size, radius, fill):
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(img).rounded_rectangle([0, 0, size[0] - 1, size[1] - 1],
                                          radius=radius, fill=fill)
    return img


def build() -> Image.Image:
    w, h = W * SCALE, H * SCALE
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # підкладка: вертикальний градієнт у скругленому прямокутнику
    grad = Image.new("RGB", (w, h))
    gd = grad.load()
    for y in range(h):
        t = y / h
        row = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        for x in range(w):
            gd[x, y] = row
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], radius=18 * SCALE, fill=255)
    img.paste(grad, (0, 0), mask)

    # фіолетове сяйво зверху ліворуч — той самий акцент, що й на сайті
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([-60 * SCALE, -90 * SCALE, 220 * SCALE, 110 * SCALE],
                                 fill=VIOLET + (70,))
    glow = glow.filter(ImageFilter.GaussianBlur(40 * SCALE))
    img.alpha_composite(Image.composite(glow, Image.new("RGBA", (w, h), (0, 0, 0, 0)),
                                        mask))

    # логотип
    icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hominka.png")
    if os.path.isfile(icon_path):
        size = 72 * SCALE
        icon = Image.open(icon_path).convert("RGBA").resize((size, size), Image.LANCZOS)
        img.alpha_composite(icon, (36 * SCALE, 40 * SCALE))

    draw = ImageDraw.Draw(img)
    draw.text((132 * SCALE, 46 * SCALE), "HOMINKA", font=font("segoeuib.ttf", 40 * SCALE),
              fill=(240, 236, 234))
    draw.text((134 * SCALE, 94 * SCALE), "чат поверх гри", font=font("segoeui.ttf", 17 * SCALE),
              fill=(154, 148, 144))

    # Доріжка, під якою bootloader пише, що саме зараз розпаковує.
    #
    # Малюємо окремим шаром і накладаємо: ImageDraw не змішує кольори з тим,
    # що вже намальовано, а ЗАМІНЮЄ пікселі — напівпрозора смуга, намальована
    # напряму, вийшла б білою.
    y = 150 * SCALE
    track = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    td = ImageDraw.Draw(track)
    td.rounded_rectangle([36 * SCALE, y, (W - 36) * SCALE, y + 5 * SCALE],
                         radius=3 * SCALE, fill=(255, 255, 255, 28))
    td.rounded_rectangle([36 * SCALE, y, 210 * SCALE, y + 5 * SCALE],
                         radius=3 * SCALE, fill=VIOLET + (235,))
    td.rounded_rectangle([190 * SCALE, y, 268 * SCALE, y + 5 * SCALE],
                         radius=3 * SCALE, fill=PINK + (200,))
    img.alpha_composite(track)

    return img.resize((W, H), Image.LANCZOS)


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "splash.png")
    build().save(out)
    print("готово:", out)
