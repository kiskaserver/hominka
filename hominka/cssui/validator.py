"""Перевірка синтаксису CSS з номерами рядків.

Це не повний парсер CSS і не має ним бути: браузер мовчки викидає те, чого не
зрозумів, і людина лишається з питанням «чому не працює».
"""

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
