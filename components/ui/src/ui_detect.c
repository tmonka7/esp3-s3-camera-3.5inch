#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "app_settings.h"
#include "app_time.h"
#include "svc_detect.h"
#include "ui_internal.h"
#include "ui_liveview.h"

#define EVENT_ROWS  5

static ui_liveview_t *s_view;
static lv_obj_t      *s_rows[EVENT_ROWS];
static lv_obj_t      *s_detect_sw;
static lv_obj_t      *s_notify_sw;
static lv_timer_t    *s_box_timer;

static lv_color_t class_color(detect_class_t cls)
{
    switch (cls) {
    case DETECT_PERSON:  return UI_COL_DANGER;
    case DETECT_VEHICLE: return UI_COL_INFO;
    default:             return UI_COL_WARN;
    }
}

static const char *class_symbol(detect_class_t cls)
{
    switch (cls) {
    case DETECT_PERSON:  return LV_SYMBOL_WARNING;
    case DETECT_VEHICLE: return LV_SYMBOL_GPS;
    default:             return LV_SYMBOL_BELL;
    }
}

/* --------------------------------------------------------------------------
 * Event list
 * ------------------------------------------------------------------------ */
static void set_row(int i, const detect_event_t *ev)
{
    lv_obj_t *row = s_rows[i];
    if (!row) {
        return;
    }

    lv_obj_t *icon  = lv_obj_get_child(row, 0);
    lv_obj_t *title = lv_obj_get_child(row, 1);
    lv_obj_t *stamp = lv_obj_get_child(row, 2);

    if (!ev) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(icon, class_symbol(ev->cls));
    lv_obj_set_style_text_color(icon, class_color(ev->cls), 0);
    lv_label_set_text(title, svc_detect_class_name(ev->cls));
    lv_label_set_text(stamp, ev->stamp);
}

static void refresh_list(void)
{
    detect_event_t events[EVENT_ROWS];
    const size_t   n = svc_detect_history(events, EVENT_ROWS);

    for (int i = 0; i < EVENT_ROWS; i++) {
        set_row(i, (i < (int)n) ? &events[i] : NULL);
    }
}

/* The box is a momentary marker: it is drawn when a detection lands and
 * cleared a few seconds later, so a stale rectangle never sits over a scene
 * where nothing is happening any more. */
static void clear_box(lv_timer_t *t)
{
    ui_liveview_set_box(s_view, false, NULL, 0, 0, 0, 0);
    /* Paused rather than given a repeat count: a one-shot LVGL timer deletes
     * itself when it fires, which would leave s_box_timer dangling. */
    lv_timer_pause(t);
}

static void on_detection(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const detect_event_t *ev = data;
    if (!ev || !ui_lock()) {
        return;
    }

    ui_liveview_set_box(s_view, true, svc_detect_class_name(ev->cls),
                        ev->x, ev->y, ev->w, ev->h);
    refresh_list();

    if (s_box_timer) {
        lv_timer_reset(s_box_timer);
        lv_timer_resume(s_box_timer);
    }

    ui_unlock();
}

/* --------------------------------------------------------------------------
 * Toggles
 * ------------------------------------------------------------------------ */
static void detect_toggled(lv_event_t *e)
{
    const bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    svc_detect_set_enabled(on);
    ui_toast(on ? "Detection on" : "Detection off");
}

static void notify_toggled(lv_event_t *e)
{
    const bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    app_settings()->detect_notify = on;
    app_settings_commit_deferred();
    ui_toast(on ? "Alerts on" : "Alerts off");
}

static void open_settings(lv_event_t *e)
{
    (void)e;
    ui_show(UI_SCREEN_SETTINGS);
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static void build_toggle_row(lv_obj_t *parent, lv_coord_t w, lv_coord_t y)
{
    lv_obj_t *bar = ui_card(parent, w, 34);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_style_pad_all(bar, 4, 0);

    lv_obj_t *l1 = ui_label(bar, "Detection", &lv_font_montserrat_12, UI_COL_TEXT);
    lv_obj_align(l1, LV_ALIGN_LEFT_MID, 2, 0);

    s_detect_sw = lv_switch_create(bar);
    lv_obj_set_size(s_detect_sw, 40, 20);
    lv_obj_align(s_detect_sw, LV_ALIGN_LEFT_MID, 66, 0);
    lv_obj_set_style_bg_color(s_detect_sw, UI_COL_PRIMARY,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_detect_sw, detect_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *l2 = ui_label(bar, "Notify", &lv_font_montserrat_12, UI_COL_TEXT);
    lv_obj_align(l2, LV_ALIGN_LEFT_MID, 118, 0);

    s_notify_sw = lv_switch_create(bar);
    lv_obj_set_size(s_notify_sw, 40, 20);
    lv_obj_align(s_notify_sw, LV_ALIGN_LEFT_MID, 166, 0);
    lv_obj_set_style_bg_color(s_notify_sw, UI_COL_PRIMARY,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_notify_sw, notify_toggled, LV_EVENT_VALUE_CHANGED, NULL);
}

static void create(lv_obj_t *parent)
{
    const lv_coord_t w = ui_width() - 2 * UI_PAD;
    const lv_coord_t h = ui_content_height() - 2 * UI_PAD;

    const lv_coord_t view_w   = 214;
    const lv_coord_t toggle_h = 34;

    s_view = ui_liveview_create(parent, view_w, h - toggle_h - UI_PAD);
    lv_obj_align(ui_liveview_obj(s_view), LV_ALIGN_TOP_LEFT, 0, 0);
    ui_liveview_set_placeholder(s_view, "Camera off");

    build_toggle_row(parent, view_w, h - toggle_h);

    /* ---- event list ---- */
    const lv_coord_t list_w = w - view_w - UI_PAD;

    lv_obj_t *card = ui_card(parent, list_w, h);
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    ui_flex_col(card, 3);

    lv_obj_t *head = ui_label(card, "Detection Events", &lv_font_montserrat_12,
                              UI_COL_MUTED);
    lv_obj_add_flag(head, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *gear = lv_btn_create(card);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, 24, 20);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_add_event_cb(gear, open_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(gear, LV_SYMBOL_SETTINGS, &lv_font_montserrat_12, UI_COL_MUTED));

    /* Push the rows below the absolutely-placed heading. */
    lv_obj_set_style_pad_top(card, 22, 0);

    for (int i = 0; i < EVENT_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 36);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xF7F9F8), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *icon = ui_label(row, LV_SYMBOL_BELL, &lv_font_montserrat_16, UI_COL_WARN);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 6, 0);

        lv_obj_t *title = ui_label(row, "", &lv_font_montserrat_12, UI_COL_TEXT);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 30, -7);

        lv_obj_t *stamp = ui_label(row, "", &lv_font_montserrat_12, UI_COL_MUTED);
        lv_obj_align(stamp, LV_ALIGN_LEFT_MID, 30, 8);

        s_rows[i] = row;
    }
}

static void on_enter(void)
{
    const app_settings_t *cfg = app_settings();

    if (svc_detect_is_enabled()) {
        lv_obj_add_state(s_detect_sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_detect_sw, LV_STATE_CHECKED);
    }
    if (cfg->detect_notify) {
        lv_obj_add_state(s_notify_sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_notify_sw, LV_STATE_CHECKED);
    }

    if (!s_box_timer) {
        s_box_timer = lv_timer_create(clear_box, 4000, NULL);
    }
    lv_timer_pause(s_box_timer);

    refresh_list();
    ui_liveview_attach_camera(s_view);
    app_event_subscribe(APP_EVT_DETECTION, on_detection, NULL);
}

static void on_leave(void)
{
    app_event_unsubscribe(APP_EVT_DETECTION, on_detection);
    ui_liveview_detach(s_view);
    ui_liveview_set_box(s_view, false, NULL, 0, 0, 0, 0);
    if (s_box_timer) {
        lv_timer_pause(s_box_timer);
    }
}

const ui_screen_def_t ui_screen_detect_def = {
    .title    = "Detection",
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
