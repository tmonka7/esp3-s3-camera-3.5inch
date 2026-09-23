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
#include "bsp_rfid.h"
#include "svc_access.h"
#include "svc_face.h"
#include "svc_webcam.h"
#include "svc_detect.h"
#include "svc_modbus.h"
#include "ui_internal.h"

#define CAT_W  108

typedef enum {
    CAT_SYSTEM = 0,
    CAT_NETWORK,
    CAT_CAMERA,
    CAT_DETECTION,
    CAT_ACCESS,
    CAT_MODBUS,
    CAT_ABOUT,
    CAT_COUNT,
} settings_cat_t;

static const ui_str_t k_cat_names[CAT_COUNT] = {
    UI_STR_SET_CAT_SYSTEM, UI_STR_SET_CAT_NETWORK, UI_STR_SET_CAT_CAMERA,
    UI_STR_SET_CAT_DETECT, UI_STR_SET_CAT_ACCESS,  UI_STR_SET_CAT_MODBUS,
    UI_STR_SET_CAT_ABOUT,
};
static const char *k_cat_icons[CAT_COUNT] = {
    LV_SYMBOL_SETTINGS, LV_SYMBOL_WIFI, LV_SYMBOL_IMAGE,
    LV_SYMBOL_EYE_OPEN, LV_SYMBOL_BELL, LV_SYMBOL_LIST, LV_SYMBOL_FILE,
};

static lv_obj_t      *s_cats[CAT_COUNT];
static lv_obj_t      *s_panel;
static settings_cat_t s_cat;

static void build_panel(void);

/* --------------------------------------------------------------------------
 * Editing dialogs
 *
 * The dialogs themselves live in ui_dialog.c, shared with the credential
 * list. These wrappers just bind them to this screen, whose panel has to be
 * rebuilt after every edit so the row shows the new value.
 * ------------------------------------------------------------------------ */
typedef void (*apply_number_t)(int value);
typedef void (*apply_text_t)(const char *text);

static void edit_number(const char *title, int min, int max, int current,
                        apply_number_t apply)
{
    ui_edit_number(title, min, max, current, apply, build_panel);
}

static void edit_text(const char *title, const char *current, bool password,
                      apply_text_t apply)
{
    ui_edit_text(title, current, password, apply, build_panel);
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

/* The clock gets its own page: six rollers beat typing a timestamp into a
 * text field, and the page can show the live clock while you set it. */
static void row_datetime(lv_event_t *e)
{
    (void)e;
    ui_show(UI_SCREEN_DATETIME);
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
    edit_number(UI_T(SET_BRIGHT_PROMPT), 5, 100, app_settings()->brightness, apply_brightness);
}

static void row_sleep(lv_event_t *e)
{
    (void)e;
    edit_number(UI_T(SET_SLEEP_PROMPT), 0, 120,
                app_settings()->auto_sleep_min, apply_sleep);
}

static void row_timezone(lv_event_t *e)
{
    (void)e;
    edit_text(UI_T(SET_TZ_PROMPT), app_settings()->timezone, false,
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
            ui_toast("%s", UI_T(SET_WIFI_RESTART));
        }
    } else {
        app_net_stop();
    }
    build_panel();
}

static void row_ssid(lv_event_t *e)
{
    (void)e;
    edit_text(UI_T(SET_SSID_PROMPT), app_settings()->wifi_ssid, false, apply_ssid);
}

static void row_pass(lv_event_t *e)
{
    (void)e;
    edit_text(UI_T(SET_PASS_PROMPT), app_settings()->wifi_pass, true, apply_pass);
}

static void row_webhook(lv_event_t *e)
{
    (void)e;
    edit_text(UI_T(SET_WEBHOOK_PROMPT), app_settings()->notify_webhook, false, apply_webhook);
}

static void row_webcam(lv_event_t *e)
{
    (void)e;
    ui_show(UI_SCREEN_WEBCAM);
}

static void row_quality(lv_event_t *e)
{
    (void)e;
    edit_number(UI_T(SET_JPEG_PROMPT), 10, 63,
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
    edit_number(UI_T(SET_SENS_PROMPT), 1, 10,
                app_settings()->detect_sensitivity, apply_sensitivity);
}

static void row_cooldown(lv_event_t *e)
{
    (void)e;
    edit_number(UI_T(SET_ALERT_PROMPT), 1, 600,
                app_settings()->detect_cooldown_s, apply_cooldown);
}

static void row_clip(lv_event_t *e)
{
    (void)e;
    edit_number(UI_T(SET_CLIP_PROMPT), 5, 300,
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
    edit_number(UI_T(SET_BAUD_PROMPT), 1200, 460800,
                (int)app_settings()->mb_rtu_baud, apply_mb_baud);
}

static void row_mb_host(lv_event_t *e)
{
    (void)e;
    edit_text(UI_T(SET_HOST_PROMPT), app_settings()->mb_tcp_host, false, apply_mb_host);
}

static void row_reboot(lv_event_t *e)
{
    (void)e;
    ui_toast("%s", UI_T(SET_RESTARTING));
    /* Give the toast a moment, and the deferred settings commit a chance to
     * reach flash, before the reset. */
    app_settings_commit();
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static void confirm_reset_yes(lv_event_t *e)
{
    (void)e;
    ui_dialog_close(NULL);
    app_settings_factory_reset();
    ui_toast("%s", UI_T(SET_RESET_DONE));
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void row_factory_reset(lv_event_t *e)
{
    (void)e;

    lv_obj_t *panel = ui_dialog_shell(UI_T(SET_RESET_ASK), 116);

    ui_label(panel, UI_T(SET_RESET_BODY), UI_FONT_12, UI_COL_MUTED);

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    ui_flex_row(row, UI_PAD);
    lv_obj_set_width(ui_button_soft(row, UI_T(CANCEL), ui_dialog_close, NULL), 110);

    lv_obj_t *yes = ui_button(row, UI_T(SET_RESET), confirm_reset_yes, NULL);
    lv_obj_set_width(yes, 110);
    lv_obj_set_style_bg_color(yes, UI_COL_DANGER, 0);
}

/* --------------------------------------------------------------------------
 * Language
 *
 * The picker labels itself in every language at once -- someone who cannot
 * read the current UI still has to find their way out of it -- so the button
 * text is drawn with the face that language needs rather than the active one.
 * ------------------------------------------------------------------------ */
static const ui_str_t k_lang_names[APP_LANG_MAX] = {
    [APP_LANG_EN] = UI_STR_SET_LANG_EN,
    [APP_LANG_JA] = UI_STR_SET_LANG_JA,
};

static void lang_picked(lv_event_t *e)
{
    const app_language_t lang = (app_language_t)(intptr_t)lv_event_get_user_data(e);

    ui_dialog_close(NULL);

    if (!ui_i18n_set_language(lang)) {
        return;     /* already that language */
    }

    /* Every screen built so far is labelled in the old language, and the
     * fonts are chosen at build time too. Restarting is smaller and more
     * honest than re-labelling the tree. */
    ui_toast("%s", UI_T(SET_LANG_CHANGED));
    app_settings_commit();
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void row_language(lv_event_t *e)
{
    (void)e;

    lv_obj_t *panel = ui_dialog_shell(UI_T(SET_LANG_PROMPT), 104);

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    ui_flex_row(row, UI_PAD);

    for (int i = 0; i < APP_LANG_MAX; i++) {
        const bool active = (i == (int)ui_i18n_language());

        lv_obj_t *b = active
            ? ui_button(row, ui_tr(k_lang_names[i]), lang_picked, (void *)(intptr_t)i)
            : ui_button_soft(row, ui_tr(k_lang_names[i]), lang_picked, (void *)(intptr_t)i);

        lv_obj_set_width(b, 110);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0),
                                   ui_i18n_font((app_language_t)i, UI_FONT_SLOT_14), 0);
    }
}

/* --------------------------------------------------------------------------
 * Panel contents
 * ------------------------------------------------------------------------ */
static const char *onoff(bool v)
{
    return v ? UI_T(ON) : UI_T(OFF);
}

static void build_system(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();
    char buf[48];

    ui_list_row(p, UI_T(SET_LANGUAGE), ui_tr(k_lang_names[cfg->language]),
                row_language, NULL);

    snprintf(buf, sizeof(buf), "%u%%", cfg->brightness);
    ui_list_row(p, UI_T(SET_BRIGHTNESS), buf, row_brightness, NULL);

    if (cfg->auto_sleep_min) {
        snprintf(buf, sizeof(buf), UI_T(SET_MINUTES_FMT), cfg->auto_sleep_min);
    } else {
        strlcpy(buf, UI_T(NEVER), sizeof(buf));
    }
    ui_list_row(p, UI_T(SET_AUTO_SLEEP), buf, row_sleep, NULL);

    app_time_format(buf, sizeof(buf), "%Y-%m-%d %H:%M");
    ui_list_row(p, UI_T(SET_DATETIME), buf, row_datetime, NULL);

    ui_list_row(p, UI_T(SET_TIMEZONE), cfg->timezone, row_timezone, NULL);
    ui_list_row(p, UI_T(SET_REBOOT), UI_T(SET_RESTART), row_reboot, NULL);
    ui_list_row(p, UI_T(SET_FACTORY), UI_T(SET_RESET), row_factory_reset, NULL);
}

static void build_network(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();

    app_wifi_state_t st;
    app_net_get_state(&st);

    ui_list_row(p, UI_T(SET_WIFI), onoff(cfg->wifi_enabled), row_wifi_toggle, NULL);
    ui_list_row(p, UI_T(SET_SSID), cfg->wifi_ssid[0] ? cfg->wifi_ssid : UI_T(NOT_SET),
                row_ssid, NULL);
    ui_list_row(p, UI_T(SET_PASSWORD), cfg->wifi_pass[0] ? "********" : UI_T(EMPTY_PAREN),
                row_pass, NULL);
    ui_list_row(p, UI_T(SET_IP), st.ip[0] ? st.ip : "--", NULL, NULL);

    char rssi[24];
    if (st.state == APP_LINK_UP) {
        snprintf(rssi, sizeof(rssi), "%d dBm", st.rssi);
    } else {
        snprintf(rssi, sizeof(rssi), "--");
    }
    ui_list_row(p, UI_T(SET_SIGNAL), rssi, NULL, NULL);

    ui_list_row(p, UI_T(SET_WEBHOOK),
                cfg->notify_webhook[0] ? UI_T(CONFIGURED) : UI_T(EMPTY_PAREN),
                row_webhook, NULL);
}

static void build_camera(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();
    char buf[32];

    const framesize_t fs = bsp_camera_get_framesize();
    snprintf(buf, sizeof(buf), "%ux%u", resolution[fs].width, resolution[fs].height);
    ui_list_row(p, UI_T(SET_RESOLUTION), buf, NULL, NULL);

    snprintf(buf, sizeof(buf), "%u", cfg->cam_quality);
    ui_list_row(p, UI_T(SET_JPEG_Q), buf, row_quality, NULL);

    ui_list_row(p, UI_T(SET_VFLIP),   onoff(cfg->cam_vflip),   row_flip,   NULL);
    ui_list_row(p, UI_T(SET_HMIRROR), onoff(cfg->cam_hmirror), row_mirror, NULL);

    if (svc_webcam_running()) {
        snprintf(buf, sizeof(buf), UI_T(SET_WEB_ON_FMT),
                 (unsigned)(cfg->webcam_port ? cfg->webcam_port : 81));
    } else {
        strlcpy(buf, UI_T(SET_WEB_OFF), sizeof(buf));
    }
    ui_list_row(p, UI_T(SET_WEB_STREAM), buf, row_webcam, NULL);

    const sensor_t *s = bsp_camera_sensor();
    if (s) {
        snprintf(buf, sizeof(buf), "0x%04X", s->id.PID);
        ui_list_row(p, UI_T(SET_SENSOR), buf, NULL, NULL);
    } else {
        ui_list_row(p, UI_T(SET_SENSOR), UI_T(NOT_DETECTED), NULL, NULL);
    }
}

static void build_detection(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();
    char buf[32];

    ui_list_row(p, UI_T(SET_DETECTION), onoff(svc_detect_is_enabled()),
                row_detect_toggle, NULL);

    snprintf(buf, sizeof(buf), UI_T(SET_SENS_FMT), cfg->detect_sensitivity);
    ui_list_row(p, UI_T(SET_SENSITIVITY), buf, row_sensitivity, NULL);

    snprintf(buf, sizeof(buf), UI_T(SET_SECONDS_FMT), cfg->detect_cooldown_s);
    ui_list_row(p, UI_T(SET_ALERT_GAP), buf, row_cooldown, NULL);

    ui_list_row(p, UI_T(SET_REC_ON_DET), onoff(cfg->detect_record_clip),
                row_record_toggle, NULL);

    snprintf(buf, sizeof(buf), UI_T(SET_SECONDS_FMT), cfg->clip_seconds);
    ui_list_row(p, UI_T(SET_CLIP_LEN), buf, row_clip, NULL);
}


/* --------------------------------------------------------------------------
 * Access control
 * ------------------------------------------------------------------------ */
static void apply_strike(int v)
{
    app_settings()->access_strike_ms = (uint16_t)(v * 1000);
    app_settings_commit();
}

static void apply_face_threshold(int v)
{
    app_settings()->access_face_threshold = (uint8_t)v;
    app_settings_commit();
}

static void apply_max_failures(int v)
{
    app_settings()->access_max_failures = (uint8_t)v;
    app_settings_commit();
}

static void apply_lockout(int v)
{
    app_settings()->access_lockout_s = (uint16_t)v;
    app_settings_commit();
}

static void row_card_toggle(lv_event_t *e)
{
    (void)e;
    app_settings()->access_card_enabled = !app_settings()->access_card_enabled;
    app_settings_commit();
    build_panel();
}

static void row_face_toggle(lv_event_t *e)
{
    (void)e;
    app_settings_t *cfg = app_settings();

    /* Switching it on with nothing behind it would be a silent no-op, and the
     * user would reasonably assume the door now opens to their face. */
    if (!cfg->access_face_enabled && !svc_face_ready()) {
        ui_toast("%s", UI_T(SET_FACE_NO_START));
        return;
    }
    if (!cfg->access_face_enabled && svc_face_subject_count() == 0) {
        /* Switching it on with nobody enrolled would arm a door that can
         * never open on a face, and look broken doing it. */
        ui_toast("%s", UI_T(SET_ENROL_FIRST));
        ui_show(UI_SCREEN_FACES);
        return;
    }

    cfg->access_face_enabled = !cfg->access_face_enabled;
    svc_face_set_enabled(cfg->access_face_enabled);
    app_settings_commit();
    build_panel();
}

static void row_faces(lv_event_t *e)
{
    (void)e;
    ui_show(UI_SCREEN_FACES);
}

static void row_snapshot_toggle(lv_event_t *e)
{
    (void)e;
    app_settings()->access_snapshot = !app_settings()->access_snapshot;
    app_settings_commit();
    build_panel();
}

static void row_access_beep(lv_event_t *e)
{
    (void)e;
    app_settings()->access_beep = !app_settings()->access_beep;
    app_settings_commit();
    build_panel();
}

static void row_strike(lv_event_t *e)
{
    (void)e;
    edit_number(UI_T(SET_STRIKE_PROMPT), 1, 60,
                app_settings()->access_strike_ms / 1000, apply_strike);
}

static void row_face_threshold(lv_event_t *e)
{
    (void)e;
    edit_number(UI_T(SET_FACE_PROMPT), 50, 99,
                app_settings()->access_face_threshold, apply_face_threshold);
}

static void row_max_failures(lv_event_t *e)
{
    (void)e;
    edit_number(UI_T(SET_FAILS_PROMPT), 0, 20,
                app_settings()->access_max_failures, apply_max_failures);
}

static void row_lockout(lv_event_t *e)
{
    (void)e;
    edit_number(UI_T(SET_LOCKOUT_PROMPT), 5, 600,
                app_settings()->access_lockout_s, apply_lockout);
}

static void row_credentials(lv_event_t *e)
{
    (void)e;
    ui_show(UI_SCREEN_CARDS);
}

static void build_access(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();
    char buf[48];

    if (bsp_rfid_present()) {
        snprintf(buf, sizeof(buf), "MFRC522 (0x%02X)", bsp_rfid_chip_version());
    } else {
        strlcpy(buf, UI_T(NOT_DETECTED), sizeof(buf));
    }
    ui_list_row(p, UI_T(SET_READER), buf, NULL, NULL);

    snprintf(buf, sizeof(buf), UI_T(SET_STORED_FMT), (unsigned)svc_access_cred_count());
    ui_list_row(p, UI_T(SET_CREDENTIALS), buf, row_credentials, NULL);

    ui_list_row(p, UI_T(SET_CARD_ENTRY), onoff(cfg->access_card_enabled),
                row_card_toggle, NULL);

    ui_list_row(p, UI_T(SET_FACE_ENTRY),
                svc_face_ready() ? onoff(cfg->access_face_enabled)
                                 : UI_T(UNAVAILABLE_LOWER),
                row_face_toggle, NULL);

    snprintf(buf, sizeof(buf), UI_T(SET_ENROLLED_FMT), (unsigned)svc_face_subject_count());
    ui_list_row(p, UI_T(SET_FACES), buf, row_faces, NULL);

    snprintf(buf, sizeof(buf), "%u %%", cfg->access_face_threshold);
    ui_list_row(p, UI_T(SET_FACE_THRESH), buf, row_face_threshold, NULL);

    snprintf(buf, sizeof(buf), UI_T(SET_SECONDS_FMT),
             (unsigned)(cfg->access_strike_ms / 1000));
    ui_list_row(p, UI_T(SET_UNLOCK_TIME), buf, row_strike, NULL);

    if (cfg->access_max_failures) {
        snprintf(buf, sizeof(buf), "%u", cfg->access_max_failures);
    } else {
        strlcpy(buf, UI_T(NEVER_LOWER), sizeof(buf));
    }
    ui_list_row(p, UI_T(SET_LOCKOUT_AFTER), buf, row_max_failures, NULL);

    snprintf(buf, sizeof(buf), UI_T(SET_SECONDS_FMT), cfg->access_lockout_s);
    ui_list_row(p, UI_T(SET_LOCKOUT_TIME), buf, row_lockout, NULL);

    ui_list_row(p, UI_T(SET_PHOTO_ENTRY), onoff(cfg->access_snapshot),
                row_snapshot_toggle, NULL);

    ui_list_row(p, UI_T(SET_BEEP), onoff(cfg->access_beep), row_access_beep, NULL);
}
static void build_modbus(lv_obj_t *p)
{
    const app_settings_t *cfg = app_settings();
    char buf[40];

    const svc_modbus_transport_t t = svc_modbus_get_transport();
    ui_list_row(p, UI_T(SET_TRANSPORT),
                t == MB_TRANSPORT_RTU ? "RTU"
                                      : t == MB_TRANSPORT_TCP ? "TCP" : UI_T(NONE),
                NULL, NULL);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)cfg->mb_rtu_baud);
    ui_list_row(p, UI_T(SET_RTU_BAUD), buf, row_mb_baud, NULL);

    snprintf(buf, sizeof(buf), "UART%d  DE %d", BSP_RS485_UART_NUM, BSP_RS485_PIN_DE);
    ui_list_row(p, UI_T(SET_RS485), buf, NULL, NULL);

    ui_list_row(p, UI_T(SET_TCP_HOST), cfg->mb_tcp_host, row_mb_host, NULL);

    uint32_t ok = 0, failed = 0;
    svc_modbus_get_stats(&ok, &failed);
    snprintf(buf, sizeof(buf), UI_T(SET_OK_FAIL_FMT),
             (unsigned long)ok, (unsigned long)failed);
    ui_list_row(p, UI_T(SET_TRANSACTIONS), buf, NULL, NULL);
}

static void build_about(lv_obj_t *p)
{
    char buf[48];

    const esp_app_desc_t *desc = esp_app_get_description();
    ui_list_row(p, UI_T(SET_FIRMWARE), desc ? desc->version : "?", NULL, NULL);
    ui_list_row(p, UI_T(SET_IDF), desc ? desc->idf_ver : "?", NULL, NULL);
    ui_list_row(p, UI_T(SET_BOARD), "ESP32-S3-Touch-LCD-3.5-C", NULL, NULL);

    snprintf(buf, sizeof(buf), UI_T(SET_KB_FREE_FMT),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ui_list_row(p, UI_T(SET_INTERNAL_RAM), buf, NULL, NULL);

    snprintf(buf, sizeof(buf), UI_T(SET_KB_FREE_FMT),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ui_list_row(p, UI_T(SET_PSRAM), buf, NULL, NULL);

    bsp_storage_info_t info;
    if (bsp_storage_info(&info) == ESP_OK) {
        snprintf(buf, sizeof(buf), UI_T(SET_GB_FMT), info.name,
                 (unsigned)(info.total_bytes >> 30));
    } else {
        strlcpy(buf, UI_T(NOT_MOUNTED), sizeof(buf));
    }
    ui_list_row(p, UI_T(SET_TF_CARD), buf, NULL, NULL);
}

static void build_panel(void)
{
    if (!s_panel) {
        return;
    }

    lv_obj_clean(s_panel);

    lv_obj_t *head = ui_label(s_panel, ui_tr(k_cat_names[s_cat]), UI_FONT_14,
                              UI_COL_TEXT);
    (void)head;

    switch (s_cat) {
    case CAT_SYSTEM:    build_system(s_panel);    break;
    case CAT_NETWORK:   build_network(s_panel);   break;
    case CAT_CAMERA:    build_camera(s_panel);    break;
    case CAT_DETECTION: build_detection(s_panel); break;
    case CAT_ACCESS:    build_access(s_panel);    break;
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

        lv_obj_t *icon = ui_label(cat, k_cat_icons[i], UI_FONT_12, fg);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 5, 0);

        lv_obj_t *lbl = ui_label(cat, ui_tr(k_cat_names[i]), UI_FONT_12, fg);
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
    ui_dialog_close(NULL);
}

const ui_screen_def_t ui_screen_settings_def = {
    .title    = UI_STR_TITLE_SETTINGS,
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
