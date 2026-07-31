"""Генерує hominka.ico — м'яка фіолетово-рожева плитка з чат-бульбашкою та
іскоркою, у стилі stream.svitix.com. Запуск: python make_icon.py"""
import os
from PIL import Image, ImageDraw

S = 1024  # рендеримо великим, потім зменшуємо для гладких країв


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return m


def make(size_out):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # діагональний градієнт фіолетовий → рожевий
    top = (124, 58, 237)     # #7c3aed violet
    bot = (236, 72, 153)     # #ec4899 pink
    grad = Image.new("RGB", (S, S))
    gd = grad.load()
    for y in range(S):
        for x in range(0, S, 1):
            t = (x + y) / (2 * S)
            gd[x, y] = lerp(top, bot, t)
    mask = rounded_mask(S, int(S * 0.235))
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)

    # м'який внутрішній глянець зверху
    gloss = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd2 = ImageDraw.Draw(gloss)
    gd2.rounded_rectangle([int(S*0.10), int(S*0.08), int(S*0.90), int(S*0.52)],
                          radius=int(S*0.18), fill=(255, 255, 255, 34))
    img.alpha_composite(Image.composite(gloss, Image.new("RGBA", (S, S), (0, 0, 0, 0)), mask))

    # біла чат-бульбашка
    bx0, by0, bx1, by1 = int(S*0.235), int(S*0.28), int(S*0.765), int(S*0.63)
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=int(S*0.12), fill=(255, 255, 255, 255))
    # хвостик бульбашки (внизу ліворуч)
    tx = int(S*0.36)
    d.polygon([(tx, by1 - int(S*0.02)), (tx + int(S*0.14), by1 - int(S*0.02)),
               (tx, by1 + int(S*0.11))], fill=(255, 255, 255, 255))

    # три крапки в бульбашці (кольорові)
    cy = (by0 + by1) // 2 - int(S*0.01)
    r = int(S*0.032)
    dots = [(int(S*0.38), (124, 58, 237)), (int(S*0.50), (168, 85, 247)),
            (int(S*0.62), (236, 72, 153))]
    for cx, col in dots:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col + (255,))

    # іскорка (4-променева зірка) вгорі праворуч
    sx, sy = int(S*0.74), int(S*0.235)
    L, w = int(S*0.075), int(S*0.020)
    d.polygon([(sx, sy - L), (sx + w, sy - w), (sx + L, sy),
               (sx + w, sy + w), (sx, sy + L), (sx - w, sy + w),
               (sx - L, sy), (sx - w, sy - w)], fill=(255, 255, 255, 245))

    return img.resize((size_out, size_out), Image.LANCZOS)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    sizes = [256, 128, 64, 48, 32, 16]
    imgs = [make(s) for s in sizes]
    out = os.path.join(here, "hominka.ico")
    imgs[0].save(out, format="ICO", sizes=[(s, s) for s in sizes])
    # також PNG для перегляду
    make(256).save(os.path.join(here, "hominka.png"))
    print("written:", out)


if __name__ == "__main__":
    main()
