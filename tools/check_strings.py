#!/usr/bin/env python3
"""Validate components/ui/include/ui_strings.def.

The catalogue is included twice per language and its strings are handed
straight to printf-family calls, so a mistake here is a crash on the panel
rather than a compiler error. This checks the things the compiler cannot:

  * every row has one entry per language, and a usable enumerator name;
  * no duplicate enumerator names;
  * both columns carry the same printf conversions in the same order;
  * every character the Japanese UI can show is in the generated font
    subset (skip with --no-font-check).

Exits non-zero on the first kind of problem found, listing all of them.

    python tools/check_strings.py
"""

import argparse
import re
import sys

import ui_strings
from ui_strings import LANGS, REPO_ROOT

# A printf conversion, minus "%%" which prints a literal '%' and consumes
# no argument.
CONVERSION = re.compile(r"""
    %
    (?P<flags>[-+ \#0']*)
    (?P<width>\*|\d+)?
    (?:\.(?P<prec>\*|\d+))?
    (?P<length>hh|h|ll|l|j|z|t|L)?
    (?P<spec>[diouxXeEfFgGaAcspn])
""", re.X)


def conversions(text):
    """The conversions in `text`, in order, normalised for comparison.

    Width and flags are presentation, not type: "%5s" and "%s" take the same
    argument, so only the length modifier and the conversion character are
    compared. A "*" width does consume an argument, so it is kept.
    """
    out = []
    i = 0
    while i < len(text):
        if text[i] != "%":
            i += 1
            continue
        if text.startswith("%%", i):
            i += 2
            continue
        m = CONVERSION.match(text, i)
        if not m:
            # A '%' that starts no conversion -- the unit sign in
            # "Brightness (%)". Recorded position-free so that a column pair
            # that both spell it the same way compares equal; report_stray()
            # is what draws attention to it.
            out.append("%")
            i += 1
            continue
        if m.group("width") == "*":
            out.append("*")
        if m.group("prec") == "*":
            out.append("*")
        out.append((m.group("length") or "") + m.group("spec"))
        i = m.end()
    return out


def font_subset():
    """The character set baked into the generated Japanese faces.

    Returns {size: set(chars)}; an unreadable or unrecognised file maps to
    None so the caller can tell "no glyphs" from "could not tell".
    """
    fonts_dir = REPO_ROOT / "components" / "ui" / "src" / "fonts"
    subsets = {}
    for path in sorted(fonts_dir.glob("ui_font_ja_*.c")):
        size = path.stem.rsplit("_", 1)[-1]
        text = path.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"--symbols\s+(.*?)\s+-o\s", text, re.S)
        subsets[size] = set(m.group(1)) if m else None
    return subsets


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-font-check", action="store_true",
                    help="skip checking the generated font subsets")
    ap.add_argument("path", nargs="?", help="catalogue to check")
    args = ap.parse_args()

    entries, problems = ui_strings.parse(args.path)

    # --- duplicate names ---
    seen = {}
    for e in entries:
        if e.name in seen:
            problems.append("line %d: duplicate name %s (first at line %d)"
                            % (e.line, e.name, seen[e.name]))
        else:
            seen[e.name] = e.line

    # --- printf conversions agree across columns ---
    for e in entries:
        base = conversions(e.values[0])
        for lang, text in zip(LANGS[1:], e.values[1:]):
            # An empty column means "not translated yet"; ui_tr() falls back
            # to English, so it carries no format risk.
            if not text:
                continue
            got = conversions(text)
            if got != base:
                problems.append(
                    "line %d: %s: %s has %s but %s has %s"
                    % (e.line, e.name,
                       LANGS[0], base or "no conversions",
                       lang, got or "no conversions"))

    # --- line counts agree across columns ---
    #
    # A "\n"-separated string is a dropdown or roller option list, and LVGL
    # reports the choice as an index into it. An extra line in one language
    # would silently select a different value rather than look wrong.
    for e in entries:
        base = e.values[0].count("\n")
        for lang, text in zip(LANGS[1:], e.values[1:]):
            if not text:
                continue
            if text.count("\n") != base:
                problems.append(
                    "line %d: %s: %s has %d line(s) but %s has %d -- an option "
                    "list must have the same entries in the same order"
                    % (e.line, e.name, LANGS[0], base + 1,
                       lang, text.count("\n") + 1))

    # --- every displayable character has a glyph ---
    if not args.no_font_check:
        # Non-ASCII can appear in any column: the language picker shows
        # "日本語" from the English UI too.
        needed = set()
        for e in entries:
            for text in e.values:
                needed.update(ch for ch in text if ord(ch) > 0x7F)

        for size, have in sorted(font_subset().items(), key=lambda kv: int(kv[0])):
            if have is None:
                problems.append("fonts: ui_font_ja_%s.c: no --symbols line; "
                                "regenerate with tools/gen_ja_fonts.py" % size)
                continue
            missing = sorted(needed - have)
            if missing:
                problems.append(
                    "fonts: ui_font_ja_%s.c is missing %d character(s): %s\n"
                    "       run: python tools/gen_ja_fonts.py"
                    % (size, len(missing), "".join(missing)))

    # --- advisory: a bare '%' is only safe in a string nobody formats ---
    stray = [e.name for e in entries if "%" in conversions(e.values[0])]
    if stray:
        print("note: bare '%%' (a unit sign, not a conversion) in: %s\n"
              "      these must be shown as-is, never passed as a printf "
              "format." % ", ".join(stray))

    if problems:
        sys.stderr.write("\n".join(problems) + "\n")
        sys.stderr.write("\n%d problem(s) in the string catalogue\n" % len(problems))
        return 1

    print("ui_strings.def OK: %d strings x %d languages"
          % (len(entries), len(LANGS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
