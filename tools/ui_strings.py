"""Shared parser for components/ui/include/ui_strings.def.

Both tools/check_strings.py and tools/gen_ja_fonts.py need to read the
catalogue, so the parsing lives here rather than in two slightly different
copies that drift apart.
"""

import pathlib
import re

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
STRINGS_DEF = REPO_ROOT / "components" / "ui" / "include" / "ui_strings.def"

# Column order in UI_STR(NAME, en, ja).
LANGS = ("en", "ja")


class Entry:
    """One UI_STR() line."""

    def __init__(self, name, values, line):
        self.name = name
        self.values = values      # list of decoded strings, parallel to LANGS
        self.line = line          # 1-based line number of the UI_STR token

    def __repr__(self):
        return "Entry(%s, line=%d)" % (self.name, self.line)


_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", "0": "\0",
    "\\": "\\", '"': '"', "'": "'",
}


def _decode(raw):
    """Turn the body of a C string literal into the text it denotes."""
    out = []
    i = 0
    while i < len(raw):
        ch = raw[i]
        if ch == "\\" and i + 1 < len(raw):
            nxt = raw[i + 1]
            out.append(_ESCAPES.get(nxt, nxt))
            i += 2
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def _split_args(body):
    """Split a UI_STR(...) body on the commas that separate arguments.

    Commas inside string literals are not separators, which is why this is
    not just body.split(",").
    """
    args = []
    cur = []
    in_str = False
    i = 0
    while i < len(body):
        ch = body[i]
        if in_str:
            if ch == "\\":
                cur.append(body[i:i + 2])
                i += 2
                continue
            if ch == '"':
                in_str = False
            cur.append(ch)
        elif ch == '"':
            in_str = True
            cur.append(ch)
        elif ch == ",":
            args.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
        i += 1
    args.append("".join(cur))
    return [a.strip() for a in args]


_LITERAL = re.compile(r'(?:u8|u|U|L)?"((?:[^"\\]|\\.)*)"')


def _literal_text(arg):
    """Concatenate the string literals in one argument, C-style.

    Returns None when the argument holds no literal at all, which is how a
    malformed line is reported upwards.
    """
    parts = _LITERAL.findall(arg)
    if not parts:
        return None
    return "".join(_decode(p) for p in parts)


def parse(path=None):
    """Read the catalogue. Returns (entries, errors)."""
    path = pathlib.Path(path) if path else STRINGS_DEF
    text = path.read_text(encoding="utf-8")

    entries = []
    errors = []
    pos = 0
    while True:
        start = text.find("UI_STR(", pos)
        if start < 0:
            break

        # Skip the macro's own definition/undef lines and comment prose.
        line_start = text.rfind("\n", 0, start) + 1
        prefix = text[line_start:start].lstrip()
        if prefix.startswith("#") or prefix.startswith("*"):
            pos = start + 7
            continue

        # Walk to the matching ')', ignoring parens inside string literals.
        i = start + len("UI_STR(")
        depth = 1
        in_str = False
        while i < len(text) and depth:
            ch = text[i]
            if in_str:
                if ch == "\\":
                    i += 2
                    continue
                if ch == '"':
                    in_str = False
            elif ch == '"':
                in_str = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            i += 1

        lineno = text.count("\n", 0, start) + 1
        if depth:
            errors.append("%s:%d: unterminated UI_STR(" % (path.name, lineno))
            break

        body = text[start + len("UI_STR("):i - 1]
        args = _split_args(body)
        pos = i

        if len(args) != 1 + len(LANGS):
            errors.append(
                "%s:%d: UI_STR takes %d arguments (name + %s), found %d"
                % (path.name, lineno, 1 + len(LANGS), "/".join(LANGS), len(args)))
            continue

        name = args[0]
        if not re.fullmatch(r"[A-Za-z_]\w*", name):
            errors.append("%s:%d: %r is not a usable enumerator name"
                          % (path.name, lineno, name))
            continue

        values = []
        bad = False
        for lang, arg in zip(LANGS, args[1:]):
            txt = _literal_text(arg)
            if txt is None:
                errors.append("%s:%d: %s: %s column is not a string literal"
                              % (path.name, lineno, name, lang))
                bad = True
                break
            values.append(txt)
        if bad:
            continue

        entries.append(Entry(name, values, lineno))

    return entries, errors
