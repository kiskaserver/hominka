"""
Підпис випусків Ed25519 — своїми руками, без сторонніх бібліотек.

Навіщо взагалі підпис. Досі цілісність оновлення трималася на двох речах:
HTTPS до update.svitix.com і sha256 у маніфесті, що приїхав звідти ж. Тобто на
одному й тому самому джерелі: хто отримає доступ до цього домену (зламаний
сервер, перехоплений DNS, помилка в конфігурації) — той підсуне і файл, і його
контрольну суму, і програма слухняно поставить чужий код на машину людини.
Підпис розриває це коло: приватний ключ лежить не на сервері, а в того, хто
випускає, і без нього манifest не пройде перевірку, хоч би що віддав сервер.

Чому Ed25519 і чому без бібліотек. У програми немає жодної залежності, крім
PySide6, і тягнути `cryptography` (десятки мегабайтів, компільований код) заради
однієї перевірки на запуск — невиправдано. Ed25519 має коротку й давно
перевірену еталонну реалізацію (RFC 8032, додаток A), яка на чистому Python
робить одну перевірку за одиниці мілісекунд. Це той рідкісний випадок, коли
«написати самому» дешевше й прозоріше.

Що НЕ треба робити з цим кодом: використовувати його для підписування чогось,
де важлива стійкість до атак по часу виконання. Тут перевіряється публічна
величина публічним ключем — таємниць у процесі немає. Приватний ключ підписує
на машині розробника (release.py), а не тут.
"""

import base64
import hashlib

# --- крива -----------------------------------------------------------------
# Числа з RFC 8032 §5.1: edwards25519.
P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
I = pow(2, (P - 1) // 4, P)          # sqrt(-1)


def _inv(x: int) -> int:
    return pow(x, P - 2, P)


def _x_recover(y: int) -> int:
    """Відновлює x за y (обидві точки на кривій відрізняються знаком x)."""
    xx = (y * y - 1) * _inv(D * y * y + 1)
    x = pow(xx, (P + 3) // 8, P)
    if (x * x - xx) % P != 0:
        x = (x * I) % P
    if x % 2 != 0:
        x = P - x
    return x


BY = 4 * _inv(5) % P
BX = _x_recover(BY)
B = (BX % P, BY % P, 1, BX * BY % P)     # базова точка в розширених координатах


def _add(p, q):
    """Додавання точок у розширених координатах (швидше за афінні)."""
    a = (p[1] - p[0]) * (q[1] - q[0]) % P
    b = (p[1] + p[0]) * (q[1] + q[0]) % P
    c = 2 * p[3] * q[3] * D % P
    dd = 2 * p[2] * q[2] % P
    e, f, g, h = b - a, dd - c, dd + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(p, e: int):
    q = (0, 1, 1, 0)
    while e > 0:
        if e & 1:
            q = _add(q, p)
        p = _add(p, p)
        e >>= 1
    return q


def _equal(p, q) -> bool:
    """Порівняння точок без ділення: (x1/z1 == x2/z2) ⇔ x1·z2 == x2·z1."""
    if (p[0] * q[2] - q[0] * p[2]) % P != 0:
        return False
    return (p[1] * q[2] - q[1] * p[2]) % P == 0


def _decode_int(b: bytes) -> int:
    return int.from_bytes(b, "little")


def _decode_point(b: bytes):
    """32 байти → точка. Кидає ValueError, якщо точки на кривій немає."""
    if len(b) != 32:
        raise ValueError("ключ або підпис не тієї довжини")
    y = _decode_int(b) & ((1 << 255) - 1)
    sign = b[31] >> 7
    x = _x_recover(y)
    if x & 1 != sign:
        x = P - x
    point = (x, y, 1, x * y % P)
    # Перевірка належності кривій: інакше підроблена точка може дати «вірний»
    # результат на кривій-двійнику.
    if not _on_curve(point):
        raise ValueError("точка не лежить на кривій")
    return point


def _on_curve(p) -> bool:
    x, y, z, t = p
    zz = z * z % P
    xx = x * x % P
    yy = y * y % P
    return (-xx * zz + yy * zz - zz * zz - D * xx * yy) % P == 0


def _h_int(*parts: bytes) -> int:
    return _decode_int(hashlib.sha512(b"".join(parts)).digest())


def verify(public_key: bytes, signature: bytes, message: bytes) -> bool:
    """Чи справді цей підпис зроблено власником public_key для message."""
    try:
        if len(signature) != 64 or len(public_key) != 32:
            return False
        a = _decode_point(public_key)
        r = _decode_point(signature[:32])
        s = _decode_int(signature[32:])
        if s >= L:                       # нормалізований підпис, без «гнучкості»
            return False
        h = _h_int(signature[:32], public_key, message) % L
        return _equal(_mul(B, s), _add(r, _mul(a, h)))
    except (ValueError, OverflowError):
        return False


def sign(secret_key: bytes, message: bytes) -> bytes:
    """Підпис. Живе тут заради release.py — програмі це не потрібно."""
    if len(secret_key) != 32:
        raise ValueError("секретний ключ має бути 32 байти")
    digest = hashlib.sha512(secret_key).digest()
    a = _clamp(digest[:32])
    prefix = digest[32:]
    r = _h_int(prefix, message) % L
    rp = encode_point(_mul(B, r))
    h = _h_int(rp, encode_point(_mul(B, a)), message) % L
    s = (r + h * a) % L
    return rp + s.to_bytes(32, "little")


def public_key(secret_key: bytes) -> bytes:
    a = _clamp(hashlib.sha512(secret_key).digest()[:32])
    return encode_point(_mul(B, a))


def _clamp(seed: bytes) -> int:
    a = bytearray(seed)
    a[0] &= 248
    a[31] &= 127
    a[31] |= 64
    return _decode_int(bytes(a))


def encode_point(p) -> bytes:
    x, y, z, _t = p
    zi = _inv(z)
    x, y = x * zi % P, y * zi % P
    return ((y & ~(1 << 255)) | ((x & 1) << 255)).to_bytes(32, "little")


# --- те, що підписуємо -----------------------------------------------------

def release_payload(manifest: dict) -> bytes:
    """Рядок, який підписується, — суть випуску і нічого зайвого.

    Свідомо НЕ підписуємо маніфест цілком: у ньому є historia і дата, які
    переписуються при кожному наступному випуску того самого каналу, і підпис
    ламався б без жодної на те причини. Підписуємо те, що вирішує, ЯКИЙ КОД
    поставить програма: продукт, канал, версію і по кожному файлу — систему,
    адресу, розмір і sha256.
    """
    files = manifest.get("files") or []
    if not files and manifest.get("file"):
        files = [manifest["file"]]
    parts = [
        str(manifest.get("product") or "hominka"),
        str(manifest.get("channel") or ""),
        str(manifest.get("version") or ""),
    ]
    for f in sorted(files, key=lambda x: str(x.get("platform"))):
        parts += [
            str(f.get("platform") or ""),
            str(f.get("url") or ""),
            str(f.get("size") or 0),
            str(f.get("sha256") or "").lower(),
        ]
    return "\n".join(parts).encode("utf-8")


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def unb64(text: str) -> bytes:
    return base64.b64decode(text.encode("ascii"), validate=True)
