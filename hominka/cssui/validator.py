"""Перевірка CSS: помилки синтаксису й попередження про непідтримуване.

Дві різні речі, і плутати їх не можна:

  * ПОМИЛКА — правило зникає цілком: незакрита дужка, оголошення без
    двокрапки. Це видно одразу й ламає все, що нижче.
  * ПОПЕРЕДЖЕННЯ — правило синтаксично бездоганне, але нативний рушій його не
    вміє (opacity, filter, animation…). Він мовчки його викине, верстка не
    зламається — а людина лишиться з питанням «чому в браузері світилося, а
    тут ні». Саме на це питання й відповідаємо заздалегідь, ще й підказуючи
    заміну.

Повний парсер CSS тут не потрібен і його немає: шукаємо рівно те, через що
людина потім не розуміє, що сталося.
"""

from .limits import UNSUPPORTED_AT, note

def validate_css(text: str):
    """Помилки CSS із номерами рядків.

    Це не повний парсер CSS і не має ним бути: браузер мовчки викидає те, чого
    не зрозумів, і людина лишається з питанням «чому не працює». Тут ловиться
    саме те, через що правило зникає цілком: незакрита дужка, коментар чи
    лапки, оголошення без двокрапки, порожній селектор.
    """
    errors = []
    depth = 0
    open_lines = []
    i, line = 0, 1
    n = len(text)
    decl_start_line = 1          # рядок, де почалося поточне оголошення
    buf = []                     # накопичене оголошення (між ; та } )
    at_rule = False

    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        # коментар
        if ch == "/" and text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end == -1:
                errors.append((line, "коментар /* не закрито"))
                break
            line += text.count("\n", i, end)
            i = end + 2
            continue
        # рядок у лапках
        if ch in "\"'":
            start = i
            end = i + 1
            while end < n and text[end] != ch:
                if text[end] == "\\":
                    end += 1
                elif text[end] == "\n":
                    break
                end += 1
            if end >= n or text[end] != ch:
                errors.append((line, "лапки %s не закрито" % ch))
                break
            # ВАЖЛИВО: лапковий рядок — це ЗНАЧЕННЯ (напр. content: ':'), тож
            # додаємо його в буфер оголошення. Без цього значення губилося, і
            # `content: ':'` виглядало як «властивість без значення».
            if not buf and text[start:end + 1].strip():
                decl_start_line = line
            buf.append(text[start:end + 1])
            i = end + 1
            continue
        if ch == "{":
            selector = "".join(buf).strip()
            if depth == 0 and not selector:
                errors.append((line, "порожній селектор перед {"))
            at_rule = selector.startswith("@")
            depth += 1
            open_lines.append(line)
            buf = []
            decl_start_line = line
            i += 1
            continue
        if ch == "}":
            rest = "".join(buf).strip().strip(";")
            if rest and depth > 0 and not at_rule:
                _check_decl(rest, decl_start_line, errors)
            if depth == 0:
                errors.append((line, "зайва } — блок не було відкрито"))
            else:
                depth -= 1
                open_lines.pop()
            buf = []
            decl_start_line = line
            i += 1
            continue
        if ch == ";":
            decl = "".join(buf).strip()
            if depth > 0 and decl:
                _check_decl(decl, decl_start_line, errors)
            buf = []
            decl_start_line = line
            i += 1
            continue
        if not buf and ch.strip():
            decl_start_line = line
        buf.append(ch)
        i += 1

    if depth > 0:
        errors.append((open_lines[-1] if open_lines else line, "блок { не закрито"))
    tail = "".join(buf).strip()
    if depth == 0 and tail and not tail.startswith("@"):
        errors.append((decl_start_line, "текст поза правилом: «%s»" % _short(tail)))
    return errors


def lint_css(text: str):
    """Попередження: (рядок, повідомлення).

    Помилок тут немає — вони окремо, у validate_css. Тут лише те, що рушій
    мовчки проігнорує, і чим це замінити.
    """
    warns = []
    seen = set()          # про ту саму властивість — один раз, а не на кожен рядок

    for line, kind, name, value in _scan(text):
        # «animation: none» програма виконує сама (вимикає появу рядка), тож
        # це не той випадок, коли правило нічого не робить.
        if kind == "prop" and name == "animation" and value == "none":
            continue
        # «filter: drop-shadow(...)» ми перекладаємо в тінь тексту — працює.
        if kind == "prop" and name == "filter" and value.startswith("drop-shadow("):
            continue
        if kind == "prop" and name == "display" and value in ("grid", "inline-grid"):
            if "display:grid" not in seen:
                seen.add("display:grid")
                what, instead = note("grid")
                warns.append((line, "«display: %s» %s. Замість: %s" % (value, what, instead)))
            continue
        if kind == "at":
            hit = UNSUPPORTED_AT.get(name)
            if hit and ("@" + name) not in seen:
                seen.add("@" + name)
                what, instead = hit
                msg = "@%s %s" % (name, what)
                if instead and instead != "—":
                    msg += ". Замість: %s" % instead
                warns.append((line, msg))
            continue
        hit = note(name)
        if hit and name not in seen:
            seen.add(name)
            what, instead = hit
            msg = "«%s» %s" % (name, what)
            if instead and instead != "—":
                msg += ". Замість: %s" % instead
            warns.append((line, msg))
    return warns


def _scan(text: str):
    """Проходить CSS і віддає (рядок, вид, ім'я) для властивостей і @-правил.

    Не парсер: нам потрібні лише імена й номери рядків, а не дерево. Але
    коментарі й лапки пропускаємо як слід — інакше «/* opacity: 1 */» дало б
    попередження про закоментоване.
    """
    i, line, n = 0, 1, len(text)
    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        if ch == "/" and text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end == -1:
                return
            line += text.count("\n", i, end)
            i = end + 2
            continue
        if ch in "\"'":
            end = i + 1
            while end < n and text[end] != ch:
                end += 2 if text[end] == "\\" else 1
            line += text.count("\n", i, min(end + 1, n))
            i = end + 1
            continue
        if ch == "@":
            j = i + 1
            while j < n and (text[j].isalpha() or text[j] == "-"):
                j += 1
            yield line, "at", text[i + 1:j].lower(), ""
            i = j
            continue
        # Ім'я властивості — те, що стоїть перед двокрапкою всередині блока.
        if ch == ":":
            j = i - 1
            while j >= 0 and (text[j].isalnum() or text[j] in "-_"):
                j -= 1
            name = text[j + 1:i].strip().lower()
            # Селектор «a:hover» теж має двокрапку — але перед ним не «{».
            if name and text.rfind("{", 0, i) > text.rfind("}", 0, i):
                # Значення теж потрібне: буває, що не діє не сама властивість,
                # а конкретне її значення («display: grid»).
                end = i + 1
                while end < n and text[end] not in ";}":
                    end += 1
                yield line, "prop", name, text[i + 1:end].strip().lower()
            i += 1
            continue
        i += 1


def _check_decl(decl: str, line: int, errors: list):
    if ":" not in decl:
        errors.append((line, "оголошення без двокрапки: «%s»" % _short(decl)))
        return
    prop, value = decl.split(":", 1)
    if not prop.strip():
        errors.append((line, "немає назви властивості перед двокрапкою"))
    elif not value.strip():
        errors.append((line, "властивість «%s» без значення" % prop.strip()))


def _short(text: str, limit: int = 40) -> str:
    text = " ".join(text.split())
    return text if len(text) <= limit else text[:limit] + "…"


# --- редактор з нумерацією рядків ------------------------------------------
