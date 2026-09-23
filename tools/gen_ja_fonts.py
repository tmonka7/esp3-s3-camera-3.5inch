#!/usr/bin/env python3
"""Regenerate components/ui/src/fonts/ui_font_ja_*.c from ui_strings.def.

Montserrat carries no kana or kanji, so the Japanese UI needs its own faces.
A full CJK face is far too big for the flash budget, so each size is a subset
holding exactly the characters the catalogue can put on screen -- which is
why this has to be re-run after any change to Japanese text.

Each generated face falls back to the Montserrat of the same size, so the
Latin, digits and LVGL symbol glyphs that are not in the subset still render
and a mixed string comes out of both faces.

Needs Node. lv_font_conv itself is vendored under third_party/lv_font_conv,
so this runs offline -- nothing is fetched from npm.

    python tools/gen_ja_fonts.py             # all sizes
    python tools/gen_ja_fonts.py --size 14   # just one
    python tools/gen_ja_fonts.py --dry-run   # print the subset and stop
"""

import argparse
import re
import shutil
import subprocess
import sys

import ui_strings
from ui_strings import REPO_ROOT

# The sizes ui_i18n.c expects a Japanese face for. 48 px is deliberately
# absent: the only 48 px label in the UI is a symbol glyph, so that slot
# stays Montserrat.
SIZES = (12, 14, 16, 20, 24, 32)

BPP = 4
TTF = REPO_ROOT / "third_party" / "fonts" / "MPLUS1p-Regular.ttf"
OUT_DIR = REPO_ROOT / "components" / "ui" / "src" / "fonts"

LV_FONT_CONV_DIR = REPO_ROOT / "third_party" / "lv_font_conv"
LV_FONT_CONV = (LV_FONT_CONV_DIR / "node_modules" / "lv_font_conv"
                / "lv_font_conv.js")

FALLBACK_NOTE = ("    /* Latin, digits and the LVGL symbol glyphs are not in "
                 "this subset;\n"
                 "     * lv_font_get_glyph_dsc() walks here for them. */\n")


def wanted_characters():
    """Every non-ASCII character the UI can display, from all columns.

    Not just the Japanese column: the language picker draws "日本語" with the
    Japanese face even while the rest of the UI is English, so that row's
    English column needs glyphs too.
    """
    entries, errors = ui_strings.parse()
    if errors:
        sys.stderr.write("\n".join(errors) + "\n")
        raise SystemExit("ui_strings.def does not parse; fix it first")

    chars = set()
    for e in entries:
        for text in e.values:
            chars.update(ch for ch in text if ord(ch) > 0x7F)
    return "".join(sorted(chars))


def converter():
    """The vendored lv_font_conv, as a [node, script] prefix.

    Deliberately not `npx lv_font_conv`: that reaches out to the npm registry,
    and this project vendors its dependencies so everything builds offline
    (see the note in .gitignore). A missing tree is an error rather than a
    quiet fall back to the network.
    """
    node = shutil.which("node") or shutil.which("node.exe")
    if not node:
        raise SystemExit("node not found on PATH -- Node.js runs lv_font_conv")

    if not LV_FONT_CONV.exists():
        raise SystemExit(
            "missing %s\n"
            "The vendored converter is not in this checkout. Restore it with:\n"
            "    cd %s && npm install --omit=dev\n"
            "(that step needs the network; the result is committed so nobody "
            "else has to repeat it)" % (LV_FONT_CONV, LV_FONT_CONV_DIR))

    return [node, str(LV_FONT_CONV)]


def patch_fallback(path, size):
    """Point the generated face at the Montserrat of the same size.

    lv_font_conv emits `.fallback = NULL`; LVGL walks that chain when a glyph
    is missing from the subset, which is what makes mixed Japanese/Latin
    strings render.
    """
    text = path.read_text(encoding="utf-8")
    replacement = "%s    .fallback = &lv_font_montserrat_%d,\n" % (FALLBACK_NOTE, size)

    patched, n = re.subn(r"[ \t]*\.fallback\s*=\s*[^,]+,\s*\n",
                         replacement, text, count=1)
    if not n:
        # Older lv_font_conv omits the field entirely; put it just before
        # .user_data, inside the same initialiser.
        patched, n = re.subn(
            r"([ \t]*\.user_data\s*=\s*NULL,\s*\n)",
            "#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9\n"
            + replacement + "#endif\n" + r"\1",
            text, count=1)
    if not n:
        raise SystemExit("%s: could not wire up the Montserrat fallback" % path.name)

    path.write_text(patched, encoding="utf-8", newline="\n")


def generate(size, symbols, dry_run=False):
    out = OUT_DIR / ("ui_font_ja_%d.c" % size)

    # Paths are passed relative to the repo root so the "Opts:" banner
    # lv_font_conv writes into the file is the same on every machine --
    # otherwise each developer's checkout path shows up as a diff.
    cmd = converter() + [
        "--font", str(TTF.relative_to(REPO_ROOT)).replace("\\", "/"),
        "--size", str(size),
        "--bpp", str(BPP),
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--no-compress",
        "--symbols", symbols,
        "-o", str(out.relative_to(REPO_ROOT)).replace("\\", "/"),
    ]

    if dry_run:
        print("would run: lv_font_conv --size %d (%d symbols)" % (size, len(symbols)))
        return

    print("generating %s (%d symbols)..." % (out.name, len(symbols)))
    subprocess.run(cmd, cwd=REPO_ROOT, check=True)
    patch_fallback(out, size)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--size", type=int, action="append", choices=SIZES,
                    help="only this size (repeatable); default is all")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the subset and the work, generate nothing")
    args = ap.parse_args()

    if not TTF.exists():
        raise SystemExit("missing %s" % TTF)

    symbols = wanted_characters()
    if not symbols:
        raise SystemExit("no non-ASCII characters in ui_strings.def -- nothing to do")

    print("%d characters in the subset:\n%s\n" % (len(symbols), symbols))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for size in (args.size or SIZES):
        generate(size, symbols, args.dry_run)

    if not args.dry_run:
        print("\ndone -- rebuild to pick up the new faces")
    return 0


if __name__ == "__main__":
    sys.exit(main())
