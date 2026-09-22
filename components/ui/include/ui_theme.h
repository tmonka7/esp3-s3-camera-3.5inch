/* Shared visual language: the green-on-off-white palette from the design,
 * plus the handful of widget recipes every screen reuses.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- palette ---------------------------------------------------------- */
#define UI_COL_BG            lv_color_hex(0xEFF3F0)
#define UI_COL_CARD          lv_color_hex(0xFFFFFF)
#define UI_COL_PRIMARY       lv_color_hex(0x3BA55D)
#define UI_COL_PRIMARY_DARK  lv_color_hex(0x2E8449)
#define UI_COL_PRIMARY_SOFT  lv_color_hex(0xE4F3E9)
#define UI_COL_TEXT          lv_color_hex(0x1F2A33)
#define UI_COL_MUTED         lv_color_hex(0x8A94A0)
#define UI_COL_TRACK         lv_color_hex(0xDDE4DF)
#define UI_COL_DANGER        lv_color_hex(0xE0574F)
#define UI_COL_WARN          lv_color_hex(0xEFA23C)
#define UI_COL_INFO          lv_color_hex(0x4A90D9)

/* ---- metrics ---------------------------------------------------------- */
#define UI_HEADER_H          38
#define UI_PAD               8
#define UI_RADIUS            10

/** Installs the base theme on the active display. Call once. */
void ui_theme_init(void);

/* ---- widget recipes --------------------------------------------------- */

/** White rounded panel with a soft shadow and no scrollbars. */
lv_obj_t *ui_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);

/** Section heading inside a card. */
lv_obj_t *ui_card_title(lv_obj_t *parent, const char *text);

/** Label with a chosen role colour and font size. */
lv_obj_t *ui_label(lv_obj_t *parent, const char *text,
                   const lv_font_t *font, lv_color_t color);

/** Green primary button with a label. */
lv_obj_t *ui_button(lv_obj_t *parent, const char *text,
                    lv_event_cb_t cb, void *user_data);

/** Light "secondary" button. */
lv_obj_t *ui_button_soft(lv_obj_t *parent, const char *text,
                         lv_event_cb_t cb, void *user_data);

/**
 * Big square tile with a glyph over a caption -- the Home dashboard and the
 * Camera page action row are both built from these.
 */
lv_obj_t *ui_tile(lv_obj_t *parent, const char *symbol, const char *caption,
                  lv_event_cb_t cb, void *user_data);

/** Small rounded status pill, e.g. "Connected" or "ON". */
lv_obj_t *ui_pill(lv_obj_t *parent, const char *text, lv_color_t color);
void      ui_pill_set(lv_obj_t *pill, const char *text, lv_color_t color);

/** Row of a settings list: label left, value + chevron right. */
lv_obj_t *ui_list_row(lv_obj_t *parent, const char *label, const char *value,
                      lv_event_cb_t cb, void *user_data);
void      ui_list_row_set_value(lv_obj_t *row, const char *value);

/** Applies a flex layout with consistent padding and gaps. */
void ui_flex_row(lv_obj_t *obj, lv_coord_t gap);
void ui_flex_col(lv_obj_t *obj, lv_coord_t gap);

#ifdef __cplusplus
}
#endif
