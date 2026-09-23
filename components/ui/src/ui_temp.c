#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_events.h"
#include "app_settings.h"
#include "svc_temp.h"
#include "ui_internal.h"

/* The arc spans 5..35 C, which covers every indoor setting without wasting
 * sweep on temperatures a house never sees. */
#define ARC_MIN_C  5
#define ARC_MAX_C  35

static lv_obj_t *s_arc;
static lv_obj_t *s_value;
static lv_obj_t *s_humidity;
static lv_obj_t *s_outdoor;
static lv_obj_t *s_setpoint;
static lv_obj_t *s_indoor_pill;
static lv_obj_t *s_tabs;
static int       s_room = 0;

static void format_c10(int16_t c10, char *out, size_t len)
{
    snprintf(out, len, "%d.%d C", c10 / 10, abs(c10 % 10));
}

static void apply_status(const temp_status_t *st)
{
    char buf[24];

    /* The tab picks which reading the gauge shows; room 0 falls back to the
     * main indoor sensor, which is what a single-sensor install has. */
    int16_t shown       = st->indoor_c10;
    bool    shown_valid = st->indoor_valid;
    if (s_room > 0 && s_room < SVC_TEMP_ROOMS) {
        shown       = st->room_c10[s_room];
        shown_valid = st->room_valid[s_room];
    }

    if (shown_valid) {
        format_c10(shown, buf, sizeof(buf));
        lv_arc_set_value(s_arc, shown / 10);
    } else {
        snprintf(buf, sizeof(buf), "--.- C");
        lv_arc_set_value(s_arc, ARC_MIN_C);
    }
    lv_label_set_text(s_value, buf);

    snprintf(buf, sizeof(buf), "%u%%", st->humidity_pct);
    lv_label_set_text(s_humidity, buf);

    if (st->outdoor_valid) {
        format_c10(st->outdoor_c10, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "--.- C");
    }
    lv_label_set_text(s_outdoor, buf);

    format_c10(st->setpoint_c10, buf, sizeof(buf));
    lv_label_set_text(s_setpoint, buf);

    if (s_indoor_pill) {
        if (st->indoor_valid) {
            format_c10(st->indoor_c10, buf, sizeof(buf));
        } else {
            strlcpy(buf, UI_T(TMP_NO_SENSOR), sizeof(buf));
        }
        ui_pill_set(s_indoor_pill, buf,
                    st->indoor_valid ? UI_COL_PRIMARY : UI_COL_MUTED);
    }
}

static void refresh(void)
{
    temp_status_t st;
    svc_temp_get(&st);
    apply_status(&st);
}

static void on_temp(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const temp_status_t *st = data;
    if (!st || !ui_lock()) {
        return;
    }
    apply_status(st);
    ui_unlock();
}

static void nudge(lv_event_t *e)
{
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);
    svc_temp_nudge_setpoint((int16_t)delta);
    refresh();
}

static void tab_clicked(lv_event_t *e)
{
    s_room = (int)(intptr_t)lv_event_get_user_data(e);

    for (int i = 0; i < SVC_TEMP_ROOMS; i++) {
        lv_obj_t *tab = lv_obj_get_child(s_tabs, i);
        if (!tab) {
            continue;
        }
        const bool on = (i == s_room);
        lv_obj_set_style_bg_color(tab, on ? UI_COL_PRIMARY : UI_COL_CARD, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(tab, 0),
                                    on ? lv_color_white() : UI_COL_MUTED, 0);
    }
    refresh();
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static void create(lv_obj_t *parent)
{
    const lv_coord_t w = ui_width() - 2 * UI_PAD;
    const lv_coord_t h = ui_content_height() - 2 * UI_PAD;

    const lv_coord_t tabs_h = 28;
    const lv_coord_t side_w = 152;
    const lv_coord_t main_w = w - side_w - UI_PAD;

    /* ---- gauge ---- */
    lv_obj_t *gauge = ui_card(parent, main_w, h - tabs_h - UI_PAD);
    lv_obj_align(gauge, LV_ALIGN_TOP_LEFT, 0, 0);

    s_arc = lv_arc_create(gauge);
    lv_obj_set_size(s_arc, 150, 150);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, ARC_MIN_C, ARC_MAX_C);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, UI_COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, UI_COL_PRIMARY, LV_PART_INDICATOR);
    /* Display only -- the setpoint is changed with the +/- buttons, which are
     * far easier to hit accurately than dragging an arc on a 3.5" panel. */
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    s_value = ui_label(gauge, "--.- C", UI_FONT_32, UI_COL_TEXT);
    lv_obj_align(s_value, LV_ALIGN_CENTER, 0, -6);

    lv_obj_t *cap = ui_label(gauge, UI_T(TMP_INDOOR), UI_FONT_12,
                             UI_COL_MUTED);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, 22);

    /* ---- right column ---- */
    const lv_coord_t card_h = (h - tabs_h - UI_PAD - 2 * UI_PAD) / 3;

    lv_obj_t *hum = ui_card(parent, side_w, card_h);
    lv_obj_set_pos(hum, main_w + UI_PAD, 0);
    lv_obj_t *hicon = ui_label(hum, LV_SYMBOL_TINT, UI_FONT_20, UI_COL_INFO);
    lv_obj_align(hicon, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_t *hcap = ui_label(hum, UI_T(TMP_HUMIDITY), UI_FONT_12, UI_COL_MUTED);
    lv_obj_align(hcap, LV_ALIGN_TOP_RIGHT, 0, 2);
    s_humidity = ui_label(hum, "--%", UI_FONT_20, UI_COL_TEXT);
    lv_obj_align(s_humidity, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *out = ui_card(parent, side_w, card_h);
    lv_obj_set_pos(out, main_w + UI_PAD, card_h + UI_PAD);
    lv_obj_t *oicon = ui_label(out, LV_SYMBOL_EYE_OPEN, UI_FONT_20, UI_COL_WARN);
    lv_obj_align(oicon, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_t *ocap = ui_label(out, UI_T(TMP_OUTDOOR), UI_FONT_12, UI_COL_MUTED);
    lv_obj_align(ocap, LV_ALIGN_TOP_RIGHT, 0, 2);
    s_outdoor = ui_label(out, "--.- C", UI_FONT_20, UI_COL_TEXT);
    lv_obj_align(s_outdoor, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *sp = ui_card(parent, side_w, card_h);
    lv_obj_set_pos(sp, main_w + UI_PAD, 2 * (card_h + UI_PAD));
    lv_obj_t *scap = ui_label(sp, UI_T(TMP_SETPOINT), UI_FONT_12, UI_COL_MUTED);
    lv_obj_align(scap, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *minus = ui_button_soft(sp, "-", nudge, (void *)(intptr_t)-5);
    lv_obj_set_size(minus, 34, 28);
    lv_obj_align(minus, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *plus = ui_button_soft(sp, "+", nudge, (void *)(intptr_t)5);
    lv_obj_set_size(plus, 34, 28);
    lv_obj_align(plus, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    s_setpoint = ui_label(sp, "--.- C", UI_FONT_16, UI_COL_TEXT);
    lv_obj_align(s_setpoint, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* ---- room tabs ---- */
    s_tabs = lv_obj_create(parent);
    lv_obj_remove_style_all(s_tabs);
    lv_obj_set_size(s_tabs, main_w, tabs_h);
    lv_obj_align(s_tabs, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_clear_flag(s_tabs, LV_OBJ_FLAG_SCROLLABLE);
    ui_flex_row(s_tabs, 4);

    s_indoor_pill = ui_pill(ui_header_slot(UI_SCREEN_TEMP), "--.- C", UI_COL_MUTED);

    const lv_coord_t tab_w = (main_w - 3 * 4) / SVC_TEMP_ROOMS;
    for (int i = 0; i < SVC_TEMP_ROOMS; i++) {
        lv_obj_t *tab = lv_btn_create(s_tabs);
        lv_obj_remove_style_all(tab);
        lv_obj_set_size(tab, tab_w, tabs_h);
        lv_obj_set_style_radius(tab, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(tab, i == 0 ? UI_COL_PRIMARY : UI_COL_CARD, 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = ui_label(tab, svc_temp_room_name(i), UI_FONT_12,
                                 i == 0 ? lv_color_white() : UI_COL_MUTED);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(tab, tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void on_enter(void)
{
    refresh();
    app_event_subscribe(APP_EVT_TEMP_UPDATE, on_temp, NULL);
}

static void on_leave(void)
{
    app_event_unsubscribe(APP_EVT_TEMP_UPDATE, on_temp);
}

const ui_screen_def_t ui_screen_temp_def = {
    .title    = UI_STR_TITLE_TEMP,
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
