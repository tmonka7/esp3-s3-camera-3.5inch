#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_events.h"
#include "app_net.h"
#include "app_time.h"
#include "svc_temp.h"
#include "ui_internal.h"
#include "ui_liveview.h"

#define SIDEBAR_W   96
#define TOPBAR_H    26

typedef struct {
    const char    *symbol;
    ui_str_t       text;
    ui_screen_id_t target;
} nav_item_t;

/* The sidebar mirrors the tile grid so either route reaches the same page. */
static const nav_item_t k_nav[] = {
    { LV_SYMBOL_HOME,      UI_STR_TITLE_HOME,      UI_SCREEN_HOME     },
    { LV_SYMBOL_IMAGE,     UI_STR_TITLE_CAMERA,    UI_SCREEN_CAMERA   },
    { LV_SYMBOL_KEYBOARD,  UI_STR_TITLE_UART,      UI_SCREEN_UART     },
    { LV_SYMBOL_LIST,      UI_STR_TITLE_MODBUS,    UI_SCREEN_MODBUS   },
    { LV_SYMBOL_CHARGE,    UI_STR_NAV_POWER,       UI_SCREEN_POWER    },
    { LV_SYMBOL_POWER,     UI_STR_NAV_SWITCHES,    UI_SCREEN_SWITCHES },
    { LV_SYMBOL_WARNING,   UI_STR_TITLE_TEMP,      UI_SCREEN_TEMP     },
    { LV_SYMBOL_SD_CARD,   UI_STR_TITLE_STORAGE,   UI_SCREEN_STORAGE  },
    { LV_SYMBOL_BELL,      UI_STR_NAV_DOOR,        UI_SCREEN_ACCESS   },
    { LV_SYMBOL_SETTINGS,  UI_STR_TITLE_SETTINGS,  UI_SCREEN_SETTINGS },
};

static const nav_item_t k_tiles[] = {
    { LV_SYMBOL_IMAGE,    UI_STR_TITLE_CAMERA,   UI_SCREEN_CAMERA   },
    { LV_SYMBOL_CHARGE,   UI_STR_NAV_POWER,      UI_SCREEN_POWER    },
    { LV_SYMBOL_POWER,    UI_STR_NAV_SWITCHES,   UI_SCREEN_SWITCHES },
    { LV_SYMBOL_BELL,     UI_STR_NAV_DOOR,       UI_SCREEN_ACCESS   },
    { LV_SYMBOL_LIST,     UI_STR_TITLE_MODBUS,   UI_SCREEN_MODBUS   },
    { LV_SYMBOL_SD_CARD,  UI_STR_TITLE_STORAGE,  UI_SCREEN_STORAGE  },
};

static lv_obj_t      *s_clock;
static lv_obj_t      *s_date;
static lv_obj_t      *s_wifi;
static lv_obj_t      *s_temp_value;
static lv_obj_t      *s_humidity;
static lv_timer_t    *s_clock_timer;
static ui_liveview_t *s_preview;

static void nav_clicked(lv_event_t *e)
{
    ui_show((ui_screen_id_t)(uintptr_t)lv_event_get_user_data(e));
}

static void tick(lv_timer_t *t)
{
    (void)t;

    char buf[32];
    app_time_format(buf, sizeof(buf), "%H:%M");
    lv_label_set_text(s_clock, buf);

    app_time_format(buf, sizeof(buf), "%Y-%m-%d %a");
    lv_label_set_text(s_date, buf);

    lv_obj_set_style_text_color(s_wifi,
                                app_net_is_connected() ? UI_COL_PRIMARY : UI_COL_MUTED, 0);

    /* The live preview carries the same timestamp the camera page shows. */
    if (s_preview) {
        app_time_format(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S");
        ui_liveview_set_caption(s_preview, buf);
    }
}

static void on_temp(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const temp_status_t *st = data;
    if (!st || !ui_lock()) {
        return;
    }

    char buf[16];
    if (st->indoor_valid) {
        snprintf(buf, sizeof(buf), "%d.%d C",
                 st->indoor_c10 / 10, abs(st->indoor_c10 % 10));
    } else {
        snprintf(buf, sizeof(buf), "--.- C");
    }
    lv_label_set_text(s_temp_value, buf);

    snprintf(buf, sizeof(buf), UI_T(HOME_HUMIDITY_FMT), st->humidity_pct);
    lv_label_set_text(s_humidity, buf);

    ui_unlock();
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static void build_topbar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, ui_width(), TOPBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COL_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_clock = ui_label(bar, "--:--", UI_FONT_16, UI_COL_TEXT);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 8, 0);

    s_date = ui_label(bar, "", UI_FONT_12, UI_COL_MUTED);
    lv_obj_align(s_date, LV_ALIGN_LEFT_MID, 62, 0);

    s_wifi = ui_label(bar, LV_SYMBOL_WIFI, UI_FONT_14, UI_COL_MUTED);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -34, 0);

    lv_obj_t *gear = lv_btn_create(bar);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, 28, TOPBAR_H);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_add_event_cb(gear, nav_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)UI_SCREEN_SETTINGS);

    lv_obj_t *icon = ui_label(gear, LV_SYMBOL_SETTINGS, UI_FONT_14, UI_COL_MUTED);
    lv_obj_center(icon);
}

static void build_sidebar(lv_obj_t *scr)
{
    lv_obj_t *side = lv_obj_create(scr);
    lv_obj_remove_style_all(side);
    lv_obj_set_size(side, SIDEBAR_W, ui_height() - TOPBAR_H);
    lv_obj_align(side, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(side, UI_COL_CARD, 0);
    lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(side, 5, 0);
    ui_flex_col(side, 1);
    lv_obj_clear_flag(side, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < sizeof(k_nav) / sizeof(k_nav[0]); i++) {
        lv_obj_t *item = lv_btn_create(side);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, LV_PCT(100), 27);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        /* Highlight the page we are on -- Home, here. */
        const bool active = (k_nav[i].target == UI_SCREEN_HOME);
        if (active) {
            lv_obj_set_style_bg_color(item, UI_COL_PRIMARY_SOFT, 0);
            lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        }

        const lv_color_t fg = active ? UI_COL_PRIMARY_DARK : UI_COL_MUTED;

        lv_obj_t *icon = ui_label(item, k_nav[i].symbol, UI_FONT_12, fg);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);

        lv_obj_t *lbl = ui_label(item, ui_tr(k_nav[i].text), UI_FONT_12, fg);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 22, 0);

        lv_obj_add_event_cb(item, nav_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)k_nav[i].target);
    }
}

static void build_main(lv_obj_t *scr)
{
    const lv_coord_t x0    = SIDEBAR_W + UI_PAD;
    const lv_coord_t width = ui_width() - x0 - UI_PAD;

    /* ---- camera preview + climate, side by side ---- */
    const lv_coord_t top_h   = 104;
    const lv_coord_t climate_w = 126;
    const lv_coord_t cam_w   = width - climate_w - UI_PAD;

    lv_obj_t *cam_card = ui_card(scr, cam_w, top_h);
    lv_obj_set_pos(cam_card, x0, TOPBAR_H + UI_PAD);
    lv_obj_set_style_pad_all(cam_card, 4, 0);
    lv_obj_add_flag(cam_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cam_card, nav_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)UI_SCREEN_CAMERA);

    s_preview = ui_liveview_create(cam_card, cam_w - 8, top_h - 8);
    lv_obj_center(ui_liveview_obj(s_preview));
    ui_liveview_set_placeholder(s_preview, UI_T(HOME_TAP_LIVE));

    lv_obj_t *climate = ui_card(scr, climate_w, top_h);
    lv_obj_set_pos(climate, x0 + cam_w + UI_PAD, TOPBAR_H + UI_PAD);
    lv_obj_add_flag(climate, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(climate, nav_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)UI_SCREEN_TEMP);

    lv_obj_t *sun = ui_label(climate, LV_SYMBOL_EYE_OPEN, UI_FONT_20, UI_COL_WARN);
    lv_obj_align(sun, LV_ALIGN_TOP_MID, 0, 0);

    s_temp_value = ui_label(climate, "--.- C", UI_FONT_24, UI_COL_TEXT);
    lv_obj_align(s_temp_value, LV_ALIGN_CENTER, 0, 2);

    lv_obj_t *cap = ui_label(climate, UI_T(HOME_INDOOR),
                             UI_FONT_12, UI_COL_MUTED);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, 22);

    s_humidity = ui_label(climate, UI_T(HOME_HUMIDITY_NA), UI_FONT_12, UI_COL_MUTED);
    lv_obj_align(s_humidity, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* ---- 3x2 tile grid ---- */
    const lv_coord_t grid_y = TOPBAR_H + UI_PAD + top_h + UI_PAD;
    const lv_coord_t tile_w = (width - 2 * UI_PAD) / 3;
    const lv_coord_t tile_h = (ui_height() - grid_y - UI_PAD * 2) / 2;

    for (size_t i = 0; i < sizeof(k_tiles) / sizeof(k_tiles[0]); i++) {
        lv_obj_t *tile = ui_tile(scr, k_tiles[i].symbol, ui_tr(k_tiles[i].text),
                                 nav_clicked, (void *)(uintptr_t)k_tiles[i].target);
        lv_obj_set_size(tile, tile_w, tile_h);
        lv_obj_set_pos(tile,
                       x0 + (lv_coord_t)(i % 3) * (tile_w + UI_PAD),
                       grid_y + (lv_coord_t)(i / 3) * (tile_h + UI_PAD));
    }
}

static void create(lv_obj_t *scr)
{
    build_topbar(scr);
    build_sidebar(scr);
    build_main(scr);
}

static void on_enter(void)
{
    if (!s_clock_timer) {
        s_clock_timer = lv_timer_create(tick, 1000, NULL);
    }
    lv_timer_resume(s_clock_timer);
    tick(NULL);

    app_event_subscribe(APP_EVT_TEMP_UPDATE, on_temp, NULL);

    /* The dashboard preview is a courtesy, not the main event: it runs the
     * camera so the thumbnail is live, and gives it up the moment another
     * screen wants it. */
    ui_liveview_attach_camera(s_preview);
}

static void on_leave(void)
{
    if (s_clock_timer) {
        lv_timer_pause(s_clock_timer);
    }
    app_event_unsubscribe(APP_EVT_TEMP_UPDATE, on_temp);
    ui_liveview_detach(s_preview);
}

const ui_screen_def_t ui_screen_home_def = {
    .title      = UI_STR_TITLE_HOME,
    .full_bleed = true,
    .create     = create,
    .on_enter   = on_enter,
    .on_leave   = on_leave,
};
