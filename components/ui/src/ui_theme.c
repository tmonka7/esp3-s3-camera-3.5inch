#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

/* Styles are shared by every widget built from a recipe below, so they are
 * created once and never freed. */
static lv_style_t s_style_card;
static lv_style_t s_style_btn;
static lv_style_t s_style_btn_pressed;
static lv_style_t s_style_btn_soft;
static lv_style_t s_style_tile;
static lv_style_t s_style_pill;
static lv_style_t s_style_row;
static bool       s_ready;

void ui_theme_init(void)
{
    if (s_ready) {
        return;
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);

    /* ---- card ---- */
    lv_style_init(&s_style_card);
    lv_style_set_bg_color(&s_style_card, UI_COL_CARD);
    lv_style_set_bg_opa(&s_style_card, LV_OPA_COVER);
    lv_style_set_radius(&s_style_card, UI_RADIUS);
    lv_style_set_border_width(&s_style_card, 0);
    lv_style_set_pad_all(&s_style_card, UI_PAD);
    /* A single soft shadow reads as depth without the fill-rate cost of a
     * large blur on a 40 MHz SPI panel. */
    lv_style_set_shadow_width(&s_style_card, 6);
    lv_style_set_shadow_ofs_y(&s_style_card, 2);
    lv_style_set_shadow_opa(&s_style_card, LV_OPA_10);
    lv_style_set_shadow_color(&s_style_card, lv_color_black());

    /* ---- primary button ---- */
    lv_style_init(&s_style_btn);
    lv_style_set_bg_color(&s_style_btn, UI_COL_PRIMARY);
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn, UI_RADIUS);
    lv_style_set_border_width(&s_style_btn, 0);
    lv_style_set_text_color(&s_style_btn, lv_color_white());
    lv_style_set_pad_hor(&s_style_btn, 12);
    lv_style_set_pad_ver(&s_style_btn, 8);

    lv_style_init(&s_style_btn_pressed);
    lv_style_set_bg_color(&s_style_btn_pressed, UI_COL_PRIMARY_DARK);
    lv_style_set_transform_width(&s_style_btn_pressed, -2);
    lv_style_set_transform_height(&s_style_btn_pressed, -2);

    /* ---- soft button ---- */
    lv_style_init(&s_style_btn_soft);
    lv_style_set_bg_color(&s_style_btn_soft, UI_COL_PRIMARY_SOFT);
    lv_style_set_bg_opa(&s_style_btn_soft, LV_OPA_COVER);
    lv_style_set_radius(&s_style_btn_soft, UI_RADIUS);
    lv_style_set_border_width(&s_style_btn_soft, 0);
    lv_style_set_text_color(&s_style_btn_soft, UI_COL_PRIMARY_DARK);
    lv_style_set_pad_hor(&s_style_btn_soft, 12);
    lv_style_set_pad_ver(&s_style_btn_soft, 8);

    /* ---- dashboard tile ---- */
    lv_style_init(&s_style_tile);
    lv_style_set_bg_color(&s_style_tile, UI_COL_PRIMARY);
    lv_style_set_bg_opa(&s_style_tile, LV_OPA_COVER);
    lv_style_set_radius(&s_style_tile, UI_RADIUS);
    lv_style_set_border_width(&s_style_tile, 0);
    lv_style_set_text_color(&s_style_tile, lv_color_white());
    lv_style_set_pad_all(&s_style_tile, 4);

    /* ---- pill ---- */
    lv_style_init(&s_style_pill);
    lv_style_set_radius(&s_style_pill, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&s_style_pill, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_pill, 0);
    lv_style_set_pad_hor(&s_style_pill, 10);
    lv_style_set_pad_ver(&s_style_pill, 3);
    lv_style_set_text_color(&s_style_pill, lv_color_white());

    /* ---- settings row ---- */
    lv_style_init(&s_style_row);
    lv_style_set_bg_opa(&s_style_row, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_style_row, 0);
    lv_style_set_border_side(&s_style_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&s_style_row, UI_COL_TRACK);
    lv_style_set_pad_hor(&s_style_row, 4);
    lv_style_set_pad_ver(&s_style_row, 9);
    lv_style_set_radius(&s_style_row, 0);

    s_ready = true;
}

void ui_flex_row(lv_obj_t *obj, lv_coord_t gap)
{
    lv_obj_set_layout(obj, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, gap, 0);
}

void ui_flex_col(lv_obj_t *obj, lv_coord_t gap)
{
    lv_obj_set_layout(obj, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(obj, gap, 0);
}

lv_obj_t *ui_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &s_style_card, 0);
    lv_obj_set_size(card, w, h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text,
                   const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text ? text : "");
    if (font) {
        lv_obj_set_style_text_font(lbl, font, 0);
    }
    lv_obj_set_style_text_color(lbl, color, 0);
    return lbl;
}

lv_obj_t *ui_card_title(lv_obj_t *parent, const char *text)
{
    return ui_label(parent, text, &lv_font_montserrat_14, UI_COL_TEXT);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_style_t *style,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, style, 0);
    lv_obj_add_style(btn, &s_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_height(btn, LV_SIZE_CONTENT);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text,
                    lv_event_cb_t cb, void *user_data)
{
    return make_button(parent, text, &s_style_btn, cb, user_data);
}

lv_obj_t *ui_button_soft(lv_obj_t *parent, const char *text,
                         lv_event_cb_t cb, void *user_data)
{
    return make_button(parent, text, &s_style_btn_soft, cb, user_data);
}

lv_obj_t *ui_tile(lv_obj_t *parent, const char *symbol, const char *caption,
                  lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *tile = lv_btn_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_add_style(tile, &s_style_tile, 0);
    lv_obj_add_style(tile, &s_style_btn_pressed, LV_STATE_PRESSED);
    ui_flex_col(tile, 2);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(tile);
    lv_label_set_text(icon, symbol ? symbol : "");
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, caption ? caption : "");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);

    if (cb) {
        lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, user_data);
    }
    return tile;
}

lv_obj_t *ui_pill(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_add_style(pill, &s_style_pill, 0);
    lv_obj_set_style_bg_color(pill, color, 0);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    return pill;
}

void ui_pill_set(lv_obj_t *pill, const char *text, lv_color_t color)
{
    if (!pill) {
        return;
    }
    lv_obj_set_style_bg_color(pill, color, 0);

    lv_obj_t *lbl = lv_obj_get_child(pill, 0);
    if (lbl && text) {
        lv_label_set_text(lbl, text);
    }
}

lv_obj_t *ui_list_row(lv_obj_t *parent, const char *label, const char *value,
                      lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &s_style_row, 0);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* Bottom hairline only; the last row's is harmless against the card edge. */
    lv_obj_set_style_border_width(row, 1, 0);

    lv_obj_t *name = ui_label(row, label, &lv_font_montserrat_14, UI_COL_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *chevron = ui_label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Child index 2 -- ui_list_row_set_value() relies on this ordering. */
    lv_obj_t *val = ui_label(row, value, &lv_font_montserrat_14, UI_COL_MUTED);
    lv_obj_align_to(val, chevron, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    }
    return row;
}

void ui_list_row_set_value(lv_obj_t *row, const char *value)
{
    if (!row || !value) {
        return;
    }
    lv_obj_t *val = lv_obj_get_child(row, 2);
    if (val) {
        lv_label_set_text(val, value);
    }
}
