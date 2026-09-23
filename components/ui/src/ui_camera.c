#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "app_settings.h"
#include "app_time.h"
#include "bsp_camera.h"
#include "svc_media.h"
#include "svc_detect.h"
#include "ui_internal.h"
#include "ui_liveview.h"

#define LIST_W   112

static ui_liveview_t *s_view;
static lv_obj_t      *s_list_items[APP_CAMERA_COUNT];
static lv_obj_t      *s_pill;
static lv_obj_t      *s_rec_badge;
static lv_timer_t    *s_timer;

static void select_camera(lv_event_t *e);
static void act_snapshot(lv_event_t *e);
static void act_record(lv_event_t *e);
static void act_flip(lv_event_t *e);
static void act_settings(lv_event_t *e);

/* --------------------------------------------------------------------------
 * Camera list
 *
 * The board has one DVP port, so the four entries are logical views of the
 * same sensor rather than four separate cameras. Selecting one is what a
 * multi-camera install would switch; here it retargets the label and the
 * recording path, and is the hook to add a networked camera later.
 * ------------------------------------------------------------------------ */
static void refresh_list_selection(void)
{
    const uint8_t active = app_settings()->cam_active;

    for (int i = 0; i < APP_CAMERA_COUNT; i++) {
        if (!s_list_items[i]) {
            continue;
        }
        const bool on = (i == active);
        lv_obj_set_style_bg_color(s_list_items[i],
                                  on ? UI_COL_PRIMARY : UI_COL_CARD, 0);
        lv_obj_set_style_bg_opa(s_list_items[i], LV_OPA_COVER, 0);

        lv_obj_t *icon = lv_obj_get_child(s_list_items[i], 0);
        lv_obj_t *name = lv_obj_get_child(s_list_items[i], 1);
        const lv_color_t fg = on ? lv_color_white() : UI_COL_TEXT;
        lv_obj_set_style_text_color(icon, fg, 0);
        lv_obj_set_style_text_color(name, fg, 0);
    }
}

static void select_camera(lv_event_t *e)
{
    const uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);

    app_settings()->cam_active = index;
    app_settings_commit_deferred();
    refresh_list_selection();
    ui_toast("%s", app_settings()->cam_names[index]);
}

/* --------------------------------------------------------------------------
 * Actions
 * ------------------------------------------------------------------------ */
static void act_snapshot(lv_event_t *e)
{
    (void)e;

    char path[96];
    if (svc_media_snapshot(path, sizeof(path)) == ESP_OK) {
        const char *name = strrchr(path, '/');
        ui_toast(UI_T(CAM_SAVED_FMT), name ? name + 1 : path);
    } else {
        ui_toast("%s", UI_T(CAM_SNAP_FAIL));
    }
}

static void act_record(lv_event_t *e)
{
    (void)e;

    if (svc_media_recording()) {
        svc_media_record_stop();
        ui_toast("%s", UI_T(CAM_REC_STOPPED));
    } else if (svc_media_record_start() == ESP_OK) {
        ui_toast("%s", UI_T(CAM_REC_STARTED));
    } else {
        ui_toast("%s", UI_T(CAM_REC_FAIL));
    }
}

static void act_flip(lv_event_t *e)
{
    (void)e;

    app_settings_t *cfg = app_settings();
    cfg->cam_vflip = !cfg->cam_vflip;
    bsp_camera_set_flip(cfg->cam_hmirror, cfg->cam_vflip);
    app_settings_commit_deferred();
    ui_toast("%s", UI_T(CAM_FLIPPED));
}

static void act_settings(lv_event_t *e)
{
    (void)e;
    ui_show(UI_SCREEN_SETTINGS);
}

/* --------------------------------------------------------------------------
 * Periodic refresh
 * ------------------------------------------------------------------------ */
static void tick(lv_timer_t *t)
{
    (void)t;

    char stamp[32];
    app_time_format(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S");
    ui_liveview_set_caption(s_view, stamp);

    const int fps = svc_media_preview_fps();
    char pill[24];
    snprintf(pill, sizeof(pill), UI_T(CAM_LIVE_FPS), fps);
    ui_pill_set(s_pill, pill, fps > 0 ? UI_COL_DANGER : UI_COL_MUTED);

    media_record_status_t rec;
    svc_media_record_status(&rec);
    if (rec.state == MEDIA_REC_RECORDING) {
        char badge[32];
        snprintf(badge, sizeof(badge), LV_SYMBOL_VIDEO "  %lu:%02lu",
                 (unsigned long)(rec.seconds / 60), (unsigned long)(rec.seconds % 60));
        lv_label_set_text(s_rec_badge, badge);
        lv_obj_clear_flag(s_rec_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_rec_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_detection(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const detect_event_t *ev = data;
    if (!ev || !ui_lock()) {
        return;
    }
    ui_liveview_set_box(s_view, true, ui_tr_detect_class(ev->cls),
                        ev->x, ev->y, ev->w, ev->h);
    ui_unlock();
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static void build_list(lv_obj_t *parent, lv_coord_t h)
{
    lv_obj_t *card = ui_card(parent, LIST_W, h);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(card, 5, 0);
    ui_flex_col(card, 4);

    ui_label(card, UI_T(CAM_LIST), UI_FONT_12, UI_COL_MUTED);

    const app_settings_t *cfg = app_settings();
    for (int i = 0; i < APP_CAMERA_COUNT; i++) {
        lv_obj_t *item = lv_btn_create(card);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, LV_PCT(100), 30);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *icon = ui_label(item, LV_SYMBOL_HOME, UI_FONT_12, UI_COL_TEXT);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);

        lv_obj_t *name = ui_label(item, cfg->cam_names[i],
                                  UI_FONT_12, UI_COL_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 22, 0);

        lv_obj_add_event_cb(item, select_camera, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        s_list_items[i] = item;
    }

    refresh_list_selection();
}

static void create(lv_obj_t *parent)
{
    const lv_coord_t h  = ui_content_height() - 2 * UI_PAD;
    const lv_coord_t vw = ui_width() - LIST_W - 3 * UI_PAD;

    build_list(parent, h);

    /* ---- live view ---- */
    const lv_coord_t actions_h = 40;
    s_view = ui_liveview_create(parent, vw, h - actions_h - 6);
    lv_obj_align(ui_liveview_obj(s_view), LV_ALIGN_TOP_RIGHT, 0, 0);
    ui_liveview_set_placeholder(s_view, UI_T(LIVE_STARTING));

    s_rec_badge = ui_label(ui_liveview_obj(s_view), "", UI_FONT_12,
                           lv_color_white());
    lv_obj_set_style_bg_color(s_rec_badge, UI_COL_DANGER, 0);
    lv_obj_set_style_bg_opa(s_rec_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_rec_badge, 6, 0);
    lv_obj_set_style_pad_ver(s_rec_badge, 2, 0);
    lv_obj_set_style_radius(s_rec_badge, 3, 0);
    lv_obj_align(s_rec_badge, LV_ALIGN_TOP_RIGHT, -6, 5);
    lv_obj_add_flag(s_rec_badge, LV_OBJ_FLAG_HIDDEN);

    /* ---- action row ---- */
    lv_obj_t *actions = lv_obj_create(parent);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, vw, actions_h);
    lv_obj_align(actions, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    ui_flex_row(actions, UI_PAD);

    const struct {
        const char   *symbol;
        ui_str_t      caption;
        lv_event_cb_t cb;
    } acts[] = {
        { LV_SYMBOL_IMAGE,    UI_STR_CAM_SNAPSHOT, act_snapshot },
        { LV_SYMBOL_VIDEO,    UI_STR_CAM_RECORD,   act_record   },
        { LV_SYMBOL_REFRESH,  UI_STR_CAM_FLIP,     act_flip     },
        { LV_SYMBOL_SETTINGS, UI_STR_CAM_SETTINGS, act_settings },
    };

    s_pill = ui_pill(ui_header_slot(UI_SCREEN_CAMERA), UI_T(CAM_LIVE), UI_COL_DANGER);

    const lv_coord_t bw = (vw - 3 * UI_PAD) / 4;
    for (size_t i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        lv_obj_t *b = ui_tile(actions, acts[i].symbol, ui_tr(acts[i].caption),
                              acts[i].cb, NULL);
        lv_obj_set_size(b, bw, actions_h);
    }
}

static void on_enter(void)
{
    ui_liveview_attach_camera(s_view);
    app_event_subscribe(APP_EVT_DETECTION, on_detection, NULL);

    if (!s_timer) {
        s_timer = lv_timer_create(tick, 500, NULL);
    }
    lv_timer_resume(s_timer);
}

static void on_leave(void)
{
    if (s_timer) {
        lv_timer_pause(s_timer);
    }
    app_event_unsubscribe(APP_EVT_DETECTION, on_detection);
    ui_liveview_detach(s_view);
    ui_liveview_set_box(s_view, false, NULL, 0, 0, 0, 0);
}

const ui_screen_def_t ui_screen_camera_def = {
    .title    = UI_STR_TITLE_CAMERA,
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
