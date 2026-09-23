/* UI language: the string catalogue, the lookup, and the fonts that go with it.
 *
 * Every user-visible string in the UI is a line in ui_strings.def and is
 * reached through UI_T(NAME). Nothing outside this header and ui_i18n.c
 * knows how many languages there are.
 *
 * The language is fixed at startup from the saved settings. Screens are
 * built once and cached, so changing it restarts the device rather than
 * trying to re-label a tree of live widgets -- see ui_i18n_set_language().
 */
#pragma once

#include "lvgl.h"
#include "app_settings.h"
#include "svc_access.h"
#include "svc_detect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the catalogue ----------------------------------------------------- *
 *
 * One enumerator per line of ui_strings.def, in file order.
 */
#define UI_STR(name, en, ja) UI_STR_##name,
typedef enum {
#include "ui_strings.def"
    UI_STR_COUNT,
} ui_str_t;
#undef UI_STR

/** The active language's text for `id`. Never NULL; an out-of-range id
 *  returns "" rather than reading past the table. */
const char *ui_tr(ui_str_t id);

const char *ui_tr_access_result(access_result_t result);
const char *ui_tr_detect_class(detect_class_t cls);

/** ui_tr() for a catalogue name written out in full: UI_T(CANCEL). */
#define UI_T(name) ui_tr(UI_STR_##name)

/* ---- fonts ------------------------------------------------------------- *
 *
 * Montserrat carries no kana or kanji, so each size is a slot that
 * ui_i18n_init() points at the face the active language needs. Widgets ask
 * for UI_FONT_14 and so on, and never name a face directly.
 *
 * Japanese text is still mostly Latin at the edges -- digits, units, "Wi-Fi",
 * and every LVGL symbol glyph. Each Japanese face therefore falls back to the
 * Montserrat of the same size (baked into the generated font, see
 * tools/gen_ja_fonts.py), so a mixed string renders from both.
 */
typedef enum {
    UI_FONT_SLOT_12 = 0,
    UI_FONT_SLOT_14,
    UI_FONT_SLOT_16,
    UI_FONT_SLOT_20,
    UI_FONT_SLOT_24,
    UI_FONT_SLOT_32,
    UI_FONT_SLOT_48,
    UI_FONT_SLOT_COUNT,
} ui_font_slot_t;

/** Resolved by ui_i18n_init(). Read through the UI_FONT_* macros below. */
extern const lv_font_t *ui_fonts[UI_FONT_SLOT_COUNT];

#define UI_FONT_12 (ui_fonts[UI_FONT_SLOT_12])
#define UI_FONT_14 (ui_fonts[UI_FONT_SLOT_14])
#define UI_FONT_16 (ui_fonts[UI_FONT_SLOT_16])
#define UI_FONT_20 (ui_fonts[UI_FONT_SLOT_20])
#define UI_FONT_24 (ui_fonts[UI_FONT_SLOT_24])
#define UI_FONT_32 (ui_fonts[UI_FONT_SLOT_32])
/* 48 px is only ever used for a symbol glyph, so it stays Montserrat. */
#define UI_FONT_48 (ui_fonts[UI_FONT_SLOT_48])

/* ---- lifecycle --------------------------------------------------------- */

/** Reads the saved language and resolves the tables above. Call once, from
 *  ui_init(), before any widget is built. */
void ui_i18n_init(void);

/** The language ui_tr() is currently answering in. */
app_language_t ui_i18n_language(void);

/**
 * The face `lang` needs at `slot`, whichever language is currently active.
 * The language picker is the reason this exists: "日本語" has to be readable
 * from an English UI, where the slots all point at Montserrat.
 */
const lv_font_t *ui_i18n_font(app_language_t lang, ui_font_slot_t slot);

/** The face widgets should inherit when they do not pick one themselves --
 *  the keyboard, drop-downs, rollers and text areas. ui_theme_init() hands
 *  this to the LVGL theme. */
const lv_font_t *ui_i18n_base_font(void);

/**
 * Saves `lang` and commits it to flash. Returns true when the language
 * actually changed, which is the caller's cue to restart: the screens
 * already built are full of strings in the old language, and rebuilding
 * them is more code than a reboot is worth on a panel that boots in a
 * second.
 */
bool ui_i18n_set_language(app_language_t lang);

#ifdef __cplusplus
}
#endif
