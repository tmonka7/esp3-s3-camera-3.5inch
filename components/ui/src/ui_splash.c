#include <stdio.h>

#include "ui_internal.h"

#define APP_VERSION "v1.0.0"

static lv_obj_t *s_bar;
static lv_obj_t *s_status;

static void create(lv_obj_t *scr)
{
    /* Soft vertical wash rather than a bitmap: it costs nothing in flash and
     * matches the panel's colour range better than a scaled-down photo. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF7FAF8), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0xDCEDE2), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

    lv_obj_t *mark = ui_label(scr, LV_SYMBOL_WIFI, &lv_font_montserrat_48, UI_COL_PRIMARY);
    lv_obj_align(mark, LV_ALIGN_CENTER, 0, -78);

    lv_obj_t *title = ui_label(scr, "Home Automation", &lv_font_montserrat_32, UI_COL_TEXT);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -26);

    lv_obj_t *tag = ui_label(scr, "Smart Home  -  Smarter Life",
                             &lv_font_montserrat_14, UI_COL_MUTED);
    lv_obj_align(tag, LV_ALIGN_CENTER, 0, 4);

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 260, 6);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 52);
    lv_obj_set_style_bg_color(s_bar, UI_COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, UI_COL_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_status = ui_label(scr, "Initializing system...", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 72);

    lv_obj_t *ver = ui_label(scr, APP_VERSION, &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, -10, 8);

    lv_obj_t *board = ui_label(scr, "ESP32-S3-Touch-LCD-3.5-C",
                               &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(board, LV_ALIGN_BOTTOM_MID, 0, -8);
}

void ui_splash_progress(int percent, const char *message)
{
    if (!s_bar || !ui_lock()) {
        return;
    }

    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    lv_bar_set_value(s_bar, percent, LV_ANIM_ON);

    if (message && s_status) {
        lv_label_set_text(s_status, message);
    }

    ui_unlock();
}

const ui_screen_def_t ui_screen_splash_def = {
    .title      = "Home Automation",
    .full_bleed = true,
    .create     = create,
};
