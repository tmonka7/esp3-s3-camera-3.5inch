#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "esp_log.h"

#include "bsp_board.h"
#include "bsp_display.h"
#include "app_events.h"
#include "ui_internal.h"

static const char *TAG = "ui";

typedef struct {
    lv_obj_t *screen;        /* NULL until first shown */
    lv_obj_t *header_slot;
} screen_slot_t;

static const ui_screen_def_t *s_defs[UI_SCREEN_COUNT];
static screen_slot_t          s_slots[UI_SCREEN_COUNT];
static ui_screen_id_t         s_current  = UI_SCREEN_SPLASH;
static ui_screen_id_t         s_previous = UI_SCREEN_HOME;
static lv_obj_t              *s_toast;
static lv_timer_t            *s_toast_timer;

bool ui_lock(void)
{
    return bsp_display_lock(0);
}

void ui_unlock(void)
{
    bsp_display_unlock();
}

lv_coord_t ui_width(void)
{
    return BSP_UI_H_RES;
}

lv_coord_t ui_height(void)
{
    return BSP_UI_V_RES;
}

lv_coord_t ui_content_height(void)
{
    return BSP_UI_V_RES - UI_HEADER_H;
}

lv_obj_t *ui_header_slot(ui_screen_id_t id)
{
    return (id < UI_SCREEN_COUNT) ? s_slots[id].header_slot : NULL;
}

/* --------------------------------------------------------------------------
 * Standard chrome
 * ------------------------------------------------------------------------ */
static void back_clicked(lv_event_t *e)
{
    (void)e;
    ui_back();
}

/** Builds the header and returns the content container below it. */
static lv_obj_t *build_chrome(lv_obj_t *scr, ui_screen_id_t id, ui_str_t title)
{
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, ui_width(), UI_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, UI_COL_CARD, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 40, UI_HEADER_H);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = ui_label(back, LV_SYMBOL_LEFT, UI_FONT_16, UI_COL_TEXT);
    lv_obj_center(arrow);

    lv_obj_t *lbl = ui_label(header, ui_tr(title), UI_FONT_16, UI_COL_TEXT);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 42, 0);

    /* Right-hand slot: screens drop a status pill in here. */
    lv_obj_t *slot = lv_obj_create(header);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, 150, UI_HEADER_H - 6);
    lv_obj_align(slot, LV_ALIGN_RIGHT_MID, -UI_PAD, 0);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    ui_flex_row(slot, 6);
    /* Right-aligned, so a pill grows leftwards away from the screen edge. */
    lv_obj_set_flex_align(slot, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_slots[id].header_slot = slot;

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, ui_width(), ui_content_height());
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(content, UI_PAD, 0);
    lv_obj_set_style_bg_color(content, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    return content;
}

/* --------------------------------------------------------------------------
 * Navigation
 * ------------------------------------------------------------------------ */
static lv_obj_t *ensure_screen(ui_screen_id_t id)
{
    if (s_slots[id].screen) {
        return s_slots[id].screen;
    }

    const ui_screen_def_t *def = s_defs[id];
    if (!def || !def->create) {
        ESP_LOGE(TAG, "screen %d has no definition", (int)id);
        return NULL;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *target = def->full_bleed ? scr : build_chrome(scr, id, def->title);
    def->create(target);

    s_slots[id].screen = scr;
    ESP_LOGD(TAG, "built screen %d (%s)", (int)id, ui_tr(def->title));
    return scr;
}

void ui_show(ui_screen_id_t id)
{
    if (id >= UI_SCREEN_COUNT) {
        return;
    }

    if (!ui_lock()) {
        ESP_LOGW(TAG, "cannot take the LVGL lock");
        return;
    }

    if (id != s_current) {
        const ui_screen_def_t *leaving = s_defs[s_current];
        if (leaving && leaving->on_leave) {
            leaving->on_leave();
        }
    }

    lv_obj_t *scr = ensure_screen(id);
    if (!scr) {
        ui_unlock();
        return;
    }

    if (id != s_current) {
        s_previous = s_current;
    }
    s_current = id;

    lv_scr_load(scr);

    const ui_screen_def_t *entering = s_defs[id];
    if (entering && entering->on_enter) {
        entering->on_enter();
    }

    ui_unlock();
}

void ui_back(void)
{
    /* Home is the root: backing out of it would be a dead end. */
    ui_show(s_current == UI_SCREEN_HOME ? UI_SCREEN_HOME : s_previous);
}

ui_screen_id_t ui_current_screen(void)
{
    return s_current;
}

/* --------------------------------------------------------------------------
 * Toast
 * ------------------------------------------------------------------------ */
static void toast_expire(lv_timer_t *t)
{
    (void)t;
    if (s_toast) {
        lv_obj_del(s_toast);
        s_toast = NULL;
    }
    s_toast_timer = NULL;
}

void ui_toast(const char *fmt, ...)
{
    char text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    if (!ui_lock()) {
        return;
    }

    if (s_toast) {
        lv_obj_del(s_toast);
        s_toast = NULL;
    }
    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }

    /* Parented to the active screen so it disappears with a screen change
     * rather than floating over the next one. */
    s_toast = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_style_bg_color(s_toast, UI_COL_TEXT, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_toast, 14, 0);
    lv_obj_set_style_pad_ver(s_toast, 7, 0);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_t *lbl = ui_label(s_toast, text, UI_FONT_14, lv_color_white());
    lv_obj_center(lbl);

    s_toast_timer = lv_timer_create(toast_expire, 2200, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);

    ui_unlock();
}

/* --------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------ */
esp_err_t ui_init(void)
{
    s_defs[UI_SCREEN_SPLASH]   = &ui_screen_splash_def;
    s_defs[UI_SCREEN_HOME]     = &ui_screen_home_def;
    s_defs[UI_SCREEN_CAMERA]   = &ui_screen_camera_def;
    s_defs[UI_SCREEN_UART]     = &ui_screen_uart_def;
    s_defs[UI_SCREEN_MODBUS]   = &ui_screen_modbus_def;
    s_defs[UI_SCREEN_POWER]    = &ui_screen_power_def;
    s_defs[UI_SCREEN_SWITCHES] = &ui_screen_switches_def;
    s_defs[UI_SCREEN_TEMP]     = &ui_screen_temp_def;
    s_defs[UI_SCREEN_DETECT]   = &ui_screen_detect_def;
    s_defs[UI_SCREEN_STORAGE]  = &ui_screen_storage_def;
    s_defs[UI_SCREEN_PLAYBACK] = &ui_screen_playback_def;
    s_defs[UI_SCREEN_SETTINGS] = &ui_screen_settings_def;
    s_defs[UI_SCREEN_DATETIME] = &ui_screen_datetime_def;
    s_defs[UI_SCREEN_ACCESS]   = &ui_screen_access_def;
    s_defs[UI_SCREEN_CARDS]    = &ui_screen_cards_def;
    s_defs[UI_SCREEN_FACES]    = &ui_screen_faces_def;
    s_defs[UI_SCREEN_WEBCAM]   = &ui_screen_webcam_def;

    /* Before anything is built: the language decides which font every
     * widget below is going to ask for. */
    ui_i18n_init();

    if (!ui_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    ui_theme_init();
    ui_unlock();

    ui_show(UI_SCREEN_SPLASH);
    return ESP_OK;
}
