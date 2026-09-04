"""Генерує splash.png — заставку, яку видно, поки програма розпаковується.

Збірка одним файлом розкладає ~300 МБ у тимчасову теку при кожному запуску, і
до появи вікна минає кілька секунд. Заставка — єдине, що в цей час каже «воно
запускається»; під нею bootloader сам пише, що саме зараз розпаковує, а перед
самою появою вікна текст міняємо на свій.

Запуск: python make_splash.py [версія] [канал]
    напр. python make_splash.py 2.7.0 beta  →  на заставці «2.7.0-beta»
Без аргументів версію бере з hominka/version.py, канал не показує.
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

# Розмір навмисно великий.
#
# Заставку показує Tk, і вікно це не знає про масштабування екрана: у Windows
# зі 125-150 % система розтягує його сама, і дрібна картинка перетворюється на
# кашу з пікселів. Що більший оригінал — то менше видно розтягування. Верхня
# межа — max_img_size у Hominka_one.spec, більше PyInstaller стисне сам.
W, H = 720, 360
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


def build(badge: str = "") -> Image.Image:
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
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], radius=26 * SCALE, fill=255)
    img.paste(grad, (0, 0), mask)

    # фіолетове сяйво зверху ліворуч — той самий акцент, що й на сайті
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([-90 * SCALE, -130 * SCALE, 330 * SCALE, 165 * SCALE],
                                 fill=VIOLET + (70,))
    glow = glow.filter(ImageFilter.GaussianBlur(56 * SCALE))
    img.alpha_composite(Image.composite(glow, Image.new("RGBA", (w, h), (0, 0, 0, 0)),
                                        mask))

    # логотип
    icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hominka.png")
    if os.path.isfile(icon_path):
        size = 108 * SCALE
        icon = Image.open(icon_path).convert("RGBA").resize((size, size), Image.LANCZOS)
        img.alpha_composite(icon, (54 * SCALE, 62 * SCALE))

    draw = ImageDraw.Draw(img)
    draw.text((198 * SCALE, 72 * SCALE), "HOMINKA", font=font("segoeuib.ttf", 58 * SCALE),
              fill=(240, 236, 234))
    draw.text((201 * SCALE, 142 * SCALE), "чат поверх гри", font=font("segoeui.ttf", 24 * SCALE),
              fill=(154, 148, 144))

    # Доріжка, під якою bootloader пише, що саме зараз розпаковує.
    #
    # Малюємо окремим шаром і накладаємо: ImageDraw не змішує кольори з тим,
    # що вже намальовано, а ЗАМІНЮЄ пікселі — напівпрозора смуга, намальована
    # напряму, вийшла б білою.
    # Доріжка смуги — і тільки доріжка.
    #
    # Саму смугу малює вже Tcl-скрипт заставки поверх цієї картинки (див.
    # Hominka_one.spec), бо вона має рухатися. Раніше тут було намальоване
    # «заповнення» — красиве й нерухоме, тобто просто брехня про прогрес.
    y = 232 * SCALE
    track = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(track).rounded_rectangle(
        [54 * SCALE, y, (W - 54) * SCALE, y + 7 * SCALE],
        radius=4 * SCALE, fill=(255, 255, 255, 28))
    img.alpha_composite(track)

    # Версія-канал у правому нижньому куті картки. Видно ввесь час, поки збірка
    # розпаковується, тож людина одразу бачить, ЩО саме запускається (напр.
    # «2.7.0-beta») — зручно, коли поруч живуть стабільна й бета.
    if badge:
        draw.text(((W - 54) * SCALE, 300 * SCALE), badge,
                  font=font("segoeui.ttf", 20 * SCALE),
                  fill=(150, 140, 170), anchor="rs")

    return img.resize((W, H), Image.LANCZOS)


def _default_version() -> str:
    try:
        from hominka.version import APP_VERSION
        return APP_VERSION
    except Exception:
        return ""


if __name__ == "__main__":
    version = sys.argv[1] if len(sys.argv) > 1 else _default_version()
    channel = sys.argv[2] if len(sys.argv) > 2 else ""
    badge = ("%s-%s" % (version, channel)) if (version and channel) else version
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "splash.png")
    build(badge).save(out)
    print("готово:", out, "(%s)" % (badge or "без версії"))
