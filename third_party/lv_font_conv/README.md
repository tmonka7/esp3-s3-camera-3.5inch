# lv_font_conv (vendored)

Converts a TTF into the LVGL C font tables in
`components/ui/src/fonts/`. Driven by `tools/gen_ja_fonts.py`; nothing in the
firmware build calls it.

It is committed here, `node_modules` and all, for the same reason LVGL and
esp32-camera are committed under `third_party/`: the project builds offline
and does not use a package manager at build time. `npx lv_font_conv` would
pull from the npm registry on every run, so `tools/gen_ja_fonts.py` invokes
`node third_party/lv_font_conv/node_modules/lv_font_conv/lv_font_conv.js`
directly and fails loudly if it is missing rather than reaching for the
network.

    lv_font_conv 1.5.3 -- MIT -- https://github.com/lvgl/lv_font_conv

Pinned exactly in `package.json`, because the generated tables are committed:
a different version would rewrite all six files with the same glyphs and bury
the real change in the diff.

## Restoring or upgrading

Only needed if `node_modules/` is somehow absent, or to move to a new
release. Both need the network, once, and the result is committed so nobody
else repeats it:

    cd third_party/lv_font_conv
    npm install --omit=dev

    # then, from the repo root, regenerate and check the tables in:
    python tools/gen_ja_fonts.py
    python tools/check_strings.py

## The typeface

The font it reads is `third_party/fonts/MPLUS1p-Regular.ttf` (M PLUS 1p),
under the SIL Open Font License -- see `third_party/fonts/OFL.txt`. Only the
~300 characters the UI can actually display are baked in; see the header of
`components/ui/include/ui_strings.def`.
