#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"

#include "app_events.h"
#include "app_net.h"
#include "app_settings.h"
#include "app_time.h"
#include "bsp_board.h"
#include "bsp_camera.h"
#include "bsp_display.h"
#include "bsp_storage.h"
#include "svc_detect.h"
#include "svc_modbus.h"
#include "ui_internal.h"

#define CAT_W  108

typedef enum {
    CAT_SYSTEM = 0,
    CAT_NETWORK,
    CAT_CAMERA,
    CAT_DETECTION,
    CAT_MODBUS,
    CAT_ABOUT,
    CAT_COUNT,
} settings_cat_t;

static const char *k_cat_names[CAT_COUNT] = {
    "System", "Network", "Camera", "Detection", "Modbus", "About",
};
static const char *k_cat_icons[CAT_COUNT] = {
    LV_SYMBOL_SETTINGS, LV_SYMBOL_WIFI, LV_SYMBOL_IMAGE,
    LV_SYMBOL_EYE_OPEN, LV_SYMBOL_LIST, LV_SYMBOL_FILE,
};

static lv_obj_t      *s_cats[CAT_COUNT];
static lv_obj_t      *s_panel;
static settings_cat_t s_cat;

static void build_panel(void);

/* --------------------------------------------------------------------------
 * Editing dialogs
 *
 * Two shapes cover everything on this screen: a bounded number and a line of
 * text. Both are modal so a half-finished edit cannot be left behind on a
 * screen change.
 * ------------------------------------------------------------------------ */
typedef void (*apply_number_t)(int value);
typedef void (*apply_text_t)(const char *text);

static lv_obj_t      *s_dialog;
static lv_obj_t      *s_dialog_field;
static apply_number_t s_apply_number;
static apply_text_t   s_apply_text;

static void dialog_close(lv_event_t *e)
{
    (void)e;
    if (s_dialog) {
        lv_obj_del(s_dialog);
        s_dialog       = NULL;
        s_dialog_field = NULL;
        s_apply_number = NULL;
        s_apply_text   = NULL;
    }
}

static void dialog_ok(lv_event_t *e)
{
    (void)e;
    if (!s_dialog_field) {
        return;
    }

    /* Copy what we need before the dialog is torn down: the apply callback
     * usually rebuilds the panel underneath us. */
    if (s_apply_number) {
        const int value = (int)lv_spinbox_get_value(s_dialog_field);
        const apply_number_t fn = s_apply_number;
        dialog_close(NULL);
        fn(value);
    } else if (s_apply_text) {
        char text[96];
        strlcpy(text, lv_textarea_get_text(s_dialog_field), sizeof(text));
        const apply_text_t fn = s_apply_text;
        dialog_close(NULL);
        fn(text);
    } else {
        dialog_close(NULL);
    }

    build_panel();
}

static lv_obj_t *dialog_shell(const char *title, lv_coord_t height)
{
    dialog_close(NULL);

    s_dialog = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_dialog);
    lv_obj_set_size(s_dialog, ui_width(), ui_height());
    lv_obj_set_style_bg_color(s_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_60, 0);
    lv_obj_clear_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = ui_card(s_dialog, ui_width() - 60, height);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 16);
    ui_flex_col(panel, 8);

    ui_label(panel, title, &lv_font_montserrat_14, UI_COL_TEXT);
    return panel;
}

static void edit_number(const char *title, int min, int max, int current,
                        apply_number_t apply)
{
    lv_obj_t *panel = dialog_shell(title, 132);
    s_apply_number = apply;

    s_dialog_field = lv_spinbox_create(panel);
    lv_spinbox_set_range(s_dialog_field, min, max);
    lv_spinbox_set_digit_format(s_dialog_field, 5, 0);
    lv_spinbox_set_value(s_dialog_field, current);
    lv_obj_set_size(s_dialog_field, LV_PCT(100), 34);

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    ui_flex_row(row, UI_PAD);
    lv_obj_set_width(ui_button_soft(row, "Cancel", dialog_close, NULL), 110);
    lv_obj_set_width(ui_button(row,      "Save",   dialog_ok,    NULL), 110);
}

static void edit_text(const char *title, const char *current, bool password,
                      apply_text_t apply)
{
    lv_obj_t *panel = dialog_shell(title, 118);
    s_apply_text = apply;

    s_dialog_field = lv_textarea_create(panel);
    lv_textarea_set_one_line(s_dialog_field, true);
    lv_textarea_set_password_mode(s_dialog_field, password);
    lv_textarea_set_text(s_dialog_field, current ? current : "");
    lv_obj_set_size(s_dialog_field, LV_PCT(100), 34);

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    ui_flex_row(row, UI_PAD);
    lv_obj_set_width(ui_button_soft(row, "Cancel", dialog_close, NULL), 110);
    lv_obj_set_width(ui_button(row,      "Save",   dialog_ok,    NULL), 110);

    lv_obj_t *kb = lv_keyboard_create(s_dialog);
    lv_obj_set_size(kb, ui_width(), 150);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_dialog_field);
}

/* --------------------------------------------------------------------------
 * Apply callbacks
 * ------------------------------------------------------------------------ */
static void apply_brightness(int v)
{
    app_settings()->brightness = (uint8_t)v;
    bsp_display_set_brightness(v);
    app_settings_commit();
}

static void apply_sleep(int v)
{
    app_settings()->auto_sleep_min = (uint16_t)v;
    app_settings_commit();
}

static void apply_ssid(const char *t)
{
    strlcpy(app_settings()->wifi_ssid, t, sizeof(app_settings()->wifi_ssid));
    app_settings_commit();
    app_net_reconnect();
}

static void apply_pass(const char *t)
{
    strlcpy(app_settings()->wifi_pass, t, sizeof(app_settings()->wifi_pass));
    app_settings_commit();
    app_net_reconnect();
}

static void apply_webhook(const char *t)
{
    strlcpy(app_settings()->notify_webhook, t, sizeof(app_settings()->notify_webhook));
    app_settings_commit();
}

static void apply_quality(int v)
{
    app_settings()->cam_quality = (uint8_t)v;
    bsp_camera_set_quality(v);
    app_settings_commit();
}

static void apply_sensitivity(int v)
{
    app_settings()->detect_sensitivity = (uint8_t)v;
    app_settings_commit();
}

static void apply_cooldown(int v)
{
    app_settings()->detect_cooldown_s = (uint16_t)v;
    app_settings_commit();
}

static void apply_clip(int v)
{
    app_settings()->clip_seconds = (uint16_t)v;
    app_settings_commit();
}

static void apply_mb_baud(int v)
{
    app_settings()->mb_rtu_baud = (uint32_t)v;
    app_settings_commit();
    if (svc_modbus_get_transport() == MB_TRANSPORT_RTU) {
        /* Bounce the transport so the new baud rate actually takes effect. */
        svc_modbus_set_transport(MB_TRANSPORT_NONE);
        svc_modbus_set_transport(MB_TRANSPORT_RTU);
    }
}

static void apply_mb_host(const char *t)
{
    strlcpy(app_settings()->mb_tcp_host, t, sizeof(app_settings()->mb_tcp_host));
    app_settings_commit();
}

static void apply_timezone(const char *t)
{
    strlcpy(app_settings()->timezone, t, sizeof(app_settings()->timezone));
    app_settings_commit();
    app_time_apply_timezone();
}

/* --------------------------------------------------------------------------
 * Row handlers
 * ------------------------------------------------------------------------ */
static void row_brightness(lv_event_t *e)
{
    (void)e;
    edit_number("Brightness (%)", 5, 100, app_settings()->brightness, apply_brightness);
}

static void row_sleep(lv_event_t *e)
{
    (void)e;
    edit_number("Auto sleep (minutes, 0 = never)", 0, 120,
                app_settings()->auto_sleep_min, apply_sleep);
}

static void row_timezone(lv_event_t *e)
{
    (void)e;
    edit_text("POSIX timezone (e.g. KST-9)", app_settings()->timezone, false,
              apply_timezone);
}

static void row_wifi_toggle(lv_event_t *e)
{
    (void)e;

    app_settings_t *cfg = app_settings();
    cfg->wifi_enabled = !cfg->wifi_enabled;
    app_settings_commit();

    if (cfg->wifi_enabled) {
        /* app_net_init() is only safe once; after that a reconnect is the
         * right lever, and a cold enable needs a restart to bring the stack
         * up in the right order. */
        if (app_net_reconnect() != ESP_OK) {
            ui_toast("Restart to enable Wi-Fi");
        }
    } else {
        app_net_stop();
    }
    build_panel();
}

static void row_ssid(lv_event_t *e)
{
    (void)e;
    edit_text("Wi-Fi SSID", app_settings()->wifi_ssid, false, apply_ssid);
}

static void row_pass(lv_event_t *e)
{
    (void)e;
    edit_text("Wi-Fi password", app_settings()->wifi_pass, true, apply_pass);
}

static void row_webhook(lv_event_t *e)
{
    (void)e;
    edit_text("Alert webhook URL", app_settings()->notify_webhook, false, apply_webhook);
}

static void row_quality(lv_event_t *e)
{
    (void)e;
    edit_number("JPEG quality (10 best .. 63 worst)", 10, 63,
                app_settings()->cam_quality, apply_quality);
}

static void row_flip(lv_event_t *e)
{
    (void)e;

    app_settings_t *cfg = app_settings();
    cfg->cam_vflip = !cfg->cam_vflip;
    bsp_camera_set_flip(cfg->cam_hmirror, cfg->cam_vflip);
    app_settings_commit();
    build_panel();
}

static void row_mirror(lv_event_t *e)
{
    (void)e;

    app_settings_t *cfg = app_settings();
    cfg->cam_hmirror = !cfg->cam_hmirror;
    bsp_camera_set_flip(cfg->cam_hmirror, cfg->cam_vflip);
    app_settings_commit();
    build_panel();
}

static void row_sensitivity(lv_event_t *e)
{
    (void)e;
    edit_number("Detection sensitivity (1..10)", 1, 10,
                app_settings()->detect_sensitivity, apply_sensitivity);
}

static void row_cooldown(lv_event_t *e)
{
    (void)e;
    edit_number("Seconds between alerts", 1, 600,
                app_settings()->detect_cooldown_s, apply_cooldown);
}

static void row_clip(lv_event_t *e)
{
    (void)e;
    edit_number("Auto-record clip length (s)", 5, 300,
                app_settings()->clip_seconds, apply_clip);
}

static void row_detect_toggle(lv_event_t *e)
{
    (void)e;
    svc_detect_set_enabled(!svc_detect_is_enabled());
    build_panel();
}

static void row_record_toggle(lv_event_t *e)
{
    (void)e;
    app_settings()->detect_record_clip = !app_settings()->detect_record_clip;
    app_settings_commit();
    build_panel();
}

static void row_mb_baud(lv_event_t *e)
{
    (void)e;
    edit_number("Modbus-RTU baud rate", 1200, 460800,
                (int)app_settings()->mb_rtu_baud, apply_mb_baud);
}

static void row_mb_host(lv_event_t *e)
{
    (void)e;
    edit_text("Modbus-TCP host", app_settings()->mb_tcp_host, false, apply_mb_host);
}

static void row_reboot(lv_event_t *e)
{
    (void)e;
    ui_toast("Restarting...");
    /* Give the toast a moment, and the deferred settings commit a chance to
     * reach flash, before the reset. */
    app_settings_commit();
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static void confirm_reset_yes(lv_event_t *e)
{
    (void)e;
    dialog_close(NULL);
    app_settings_factory_reset();
    ui_toast("Defaults restored - restarting");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void row_factory_reset(lv_event_t *e)
{
    (void)e;

    lv_obj_t *panel = dialog_shell("Reset every setting to defaults?", 116);

    ui_label(panel, "Switch names, bindings, Wi-Fi and the Modbus map are\n"
                    "all restored. Recordings on the TF card are kept.",
             &lv_font_montserrat_12, UI_COL_MUTED);

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    ui_flex_row(row, UI_PAD);
    lv_obj_set_width(ui_button_soft(row, "Cancel", dialog_close, NULL), 110);

    lv_obj_t *yes = ui_button(row, "Reset", confirm_reset_yes, NULL);
    lv_obj_set_width(yes, 110);
    lv_obj_set_style_bg_color(yes, UI_COL_DANGER, 0);
}

/* --------------------------------------------------------------------------
 * Panel contents
 * ------------------------------------------------------------------------ */
static const char *onoff(bool v)
{
    return v ? "On" : "Off";
}

static void build_system(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();
    char buf[48];

    snprintf(buf, sizeof(buf), "%u%%", cfg->brightness);
    ui_list_row(p, "Brightness", buf, row_brightness, NULL);

    if (cfg->auto_sleep_min) {
        snprintf(buf, sizeof(buf), "%u min", cfg->auto_sleep_min);
    } else {
        snprintf(buf, sizeof(buf), "Never");
    }
    ui_list_row(p, "Auto Sleep", buf, row_sleep, NULL);

    app_time_format(buf, sizeof(buf), "%Y-%m-%d %H:%M");
    ui_list_row(p, "Date & Time", buf, NULL, NULL);

    ui_list_row(p, "Time Zone", cfg->timezone, row_timezone, NULL);
    ui_list_row(p, "Reboot", "Restart", row_reboot, NULL);
    ui_list_row(p, "Factory Reset", "Reset", row_factory_reset, NULL);
}

static void build_network(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();

    app_wifi_state_t st;
    app_net_get_state(&st);

    ui_list_row(p, "Wi-Fi", onoff(cfg->wifi_enabled), row_wifi_toggle, NULL);
    ui_list_row(p, "SSID", cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(not set)",
                row_ssid, NULL);
    ui_list_row(p, "Password", cfg->wifi_pass[0] ? "********" : "(none)",
                row_pass, NULL);
    ui_list_row(p, "IP Address", st.ip[0] ? st.ip : "--", NULL, NULL);

    char rssi[24];
    if (st.state == APP_LINK_UP) {
        snprintf(rssi, sizeof(rssi), "%d dBm", st.rssi);
    } else {
        snprintf(rssi, sizeof(rssi), "--");
    }
    ui_list_row(p, "Signal", rssi, NULL, NULL);

    ui_list_row(p, "Alert Webhook",
                cfg->notify_webhook[0] ? "Configured" : "(none)", row_webhook, NULL);
}

static void build_camera(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();
    char buf[32];

    const framesize_t fs = bsp_camera_get_framesize();
    snprintf(buf, sizeof(buf), "%ux%u", resolution[fs].width, resolution[fs].height);
    ui_list_row(p, "Resolution", buf, NULL, NULL);

    snprintf(buf, sizeof(buf), "%u", cfg->cam_quality);
    ui_list_row(p, "JPEG Quality", buf, row_quality, NULL);

    ui_list_row(p, "Vertical Flip",   onoff(cfg->cam_vflip),   row_flip,   NULL);
    ui_list_row(p, "Horizontal Mirror", onoff(cfg->cam_hmirror), row_mirror, NULL);

    const sensor_t *s = bsp_camera_sensor();
    if (s) {
        snprintf(buf, sizeof(buf), "0x%04X", s->id.PID);
        ui_list_row(p, "Sensor", buf, NULL, NULL);
    } else {
        ui_list_row(p, "Sensor", "not detected", NULL, NULL);
    }
}

static void build_detection(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();
    char buf[32];

    ui_list_row(p, "Detection", onoff(svc_detect_is_enabled()), row_detect_toggle, NULL);

    snprintf(buf, sizeof(buf), "%u / 10", cfg->detect_sensitivity);
    ui_list_row(p, "Sensitivity", buf, row_sensitivity, NULL);

    snprintf(buf, sizeof(buf), "%u s", cfg->detect_cooldown_s);
    ui_list_row(p, "Alert Interval", buf, row_cooldown, NULL);

    ui_list_row(p, "Record on Detect", onoff(cfg->detect_record_clip),
                row_record_toggle, NULL);

    snprintf(buf, sizeof(buf), "%u s", cfg->clip_seconds);
    ui_list_row(p, "Clip Length", buf, row_clip, NULL);
}

static void build_modbus(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();
    char buf[40];

    const svc_modbus_transport_t t = svc_modbus_get_transport();
    ui_list_row(p, "Active Transport",
                t == MB_TRANSPORT_RTU ? "RTU" : t == MB_TRANSPORT_TCP ? "TCP" : "None",
                NULL, NULL);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)cfg->mb_rtu_baud);
    ui_list_row(p, "RTU Baud", buf, row_mb_baud, NULL);

    snprintf(buf, sizeof(buf), "UART%d  DE %d", BSP_RS485_UART_NUM, BSP_RS485_PIN_DE);
    ui_list_row(p, "RS485 Port", buf, NULL, NULL);

    ui_list_row(p, "TCP Host", cfg->mb_tcp_host, row_mb_host, NULL);

    uint32_t ok = 0, failed = 0;
    svc_modbus_get_stats(&ok, &failed);
    snprintf(buf, sizeof(buf), "%lu ok / %lu failed",
             (unsigned long)ok, (unsigned long)failed);
    ui_list_row(p, "Transactions", buf, NULL, NULL);
}

static void build_about(lv_obj_t *p)
{
    char buf[48];

    const esp_app_desc_t *desc = esp_app_get_description();
    ui_list_row(p, "Firmware", desc ? desc->version : "?", NULL, NULL);
    ui_list_row(p, "IDF", desc ? desc->idf_ver : "?", NULL, NULL);
    ui_list_row(p, "Board", "ESP32-S3-Touch-LCD-3.5-C", NULL, NULL);

    snprintf(buf, sizeof(buf), "%u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ui_list_row(p, "Internal RAM", buf, NULL, NULL);

    snprintf(buf, sizeof(buf), "%u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ui_list_row(p, "PSRAM", buf, NULL, NULL);

    bsp_storage_info_t info;
    if (bsp_storage_info(&info) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%s  %u GB", info.name,
                 (unsigned)(info.total_bytes >> 30));
    } else {
        snprintf(buf, sizeof(buf), "not mounted");
    }
    ui_list_row(p, "TF Card", buf, NULL, NULL);
}

static void build_panel(void)
{
    if (!s_panel) {
        return;
    }

    lv_obj_clean(s_panel);

    lv_obj_t *head = ui_label(s_panel, k_cat_names[s_cat], &lv_font_montserrat_14,
                              UI_COL_TEXT);
    (void)head;

    switch (s_cat) {
    case CAT_SYSTEM:    build_system(s_panel);    break;
    case CAT_NETWORK:   build_network(s_panel);   break;
    case CAT_CAMERA:    build_camera(s_panel);    break;
    case CAT_DETECTION: build_detection(s_panel); break;
    case CAT_MODBUS:    build_modbus(s_panel);    break;
    case CAT_ABOUT:     build_about(s_panel);     break;
    default: break;
    }
}

static void cat_clicked(lv_event_t *e)
{
    s_cat = (settings_cat_t)(intptr_t)lv_event_get_user_data(e);

    for (int i = 0; i < CAT_COUNT; i++) {
        const bool on = (i == (int)s_cat);
        lv_obj_set_style_bg_color(s_cats[i], on ? UI_COL_PRIMARY : UI_COL_CARD, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_cats[i], 0),
                                    on ? lv_color_white() : UI_COL_MUTED, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_cats[i], 1),
                                    on ? lv_color_white() : UI_COL_MUTED, 0);
    }
    build_panel();
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static void create(lv_obj_t *parent)
{
    const lv_coord_t w = ui_width() - 2 * UI_PAD;
    const lv_coord_t h = ui_content_height() - 2 * UI_PAD;

    lv_obj_t *cats = ui_card(parent, CAT_W, h);
    lv_obj_align(cats, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(cats, 5, 0);
    ui_flex_col(cats, 3);

    for (int i = 0; i < CAT_COUNT; i++) {
        lv_obj_t *cat = lv_btn_create(cats);
        lv_obj_remove_style_all(cat);
        lv_obj_set_size(cat, LV_PCT(100), 30);
        lv_obj_set_style_radius(cat, 6, 0);
        lv_obj_set_style_bg_color(cat, i == 0 ? UI_COL_PRIMARY : UI_COL_CARD, 0);
        lv_obj_set_style_bg_opa(cat, LV_OPA_COVER, 0);
        lv_obj_clear_flag(cat, LV_OBJ_FLAG_SCROLLABLE);

        const lv_color_t fg = (i == 0) ? lv_color_white() : UI_COL_MUTED;

        lv_obj_t *icon = ui_label(cat, k_cat_icons[i], &lv_font_montserrat_12, fg);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 5, 0);

        lv_obj_t *lbl = ui_label(cat, k_cat_names[i], &lv_font_montserrat_12, fg);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 24, 0);

        lv_obj_add_event_cb(cat, cat_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_cats[i] = cat;
    }

    s_panel = ui_card(parent, w - CAT_W - UI_PAD, h);
    lv_obj_align(s_panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 8, 0);
    ui_flex_col(s_panel, 0);
    /* Six rows plus a heading overflow the card on the shortest category, so
     * let this one scroll rather than clipping the last row. */
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_panel, LV_DIR_VER);
}

static void on_enter(void)
{
    build_panel();
}

static void on_leave(void)
{
    dialog_close(NULL);
}

const ui_screen_def_t ui_screen_settings_def = {
    .title    = "Settings",
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
