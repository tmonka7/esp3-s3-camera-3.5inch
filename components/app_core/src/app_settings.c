#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_events.h"
#include "app_settings.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "homeauto"
#define NVS_KEY_BLOB  "cfg"

static app_settings_t s_cfg;
static bool           s_loaded;

static const char *k_default_switch_names[APP_SWITCH_COUNT] = {
    "Living Room", "Bedroom", "Kitchen",  "Study",   "Bathroom",
    "Hallway",     "Balcony", "Garage",   "Garden",  "Light_All",
};

static const char *k_default_camera_names[APP_CAMERA_COUNT] = {
    "Front Door", "Backyard", "Garage", "Office",
};

static void load_defaults(app_settings_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = APP_SETTINGS_VERSION;

    c->language       = APP_LANG_EN;
    c->brightness     = 80;
    c->auto_sleep_min = 10;
    strlcpy(c->timezone,    "UTC0",          sizeof(c->timezone));
    strlcpy(c->ntp_server,  "pool.ntp.org",  sizeof(c->ntp_server));

    c->wifi_enabled = false;

    /* FRAMESIZE_QVGA (320x240): matches the preview area, keeps the JPEG
     * decode inside the detector's buffer, and leaves headroom for 15+ fps. */
    c->cam_framesize = 5;
    c->cam_quality   = 12;
    c->cam_hmirror   = false;
    c->cam_vflip     = true;
    c->cam_active    = 0;

    /* The web stream stays off until someone asks for it: it publishes the
     * camera to anyone who can reach the panel, which should be a decision
     * rather than a default. */
    c->webcam_enabled = false;
    c->webcam_port    = 81;

    for (int i = 0; i < APP_CAMERA_COUNT; i++) {
        strlcpy(c->cam_names[i], k_default_camera_names[i], APP_CAMERA_NAME_LEN);
    }

    c->detect_enabled     = true;
    c->detect_notify      = true;
    c->detect_sensitivity = 5;
    c->detect_cooldown_s  = 15;
    c->detect_record_clip = true;

    c->uart_baud     = 115200;
    c->uart_databits = 8;
    c->uart_parity   = 0;
    c->uart_stopbits = 1;
    c->uart_hex_view = false;
    c->uart_log_to_sd = false;

    c->mb_rtu_enabled = true;
    c->mb_rtu_baud    = 9600;
    c->mb_rtu_parity  = 0;
    c->mb_tcp_enabled = false;
    strlcpy(c->mb_tcp_host, "192.168.1.10", sizeof(c->mb_tcp_host));
    c->mb_tcp_port = 502;
    c->mb_poll_ms  = 1000;

    /* A plausible starting map for a 40001-based energy meter. The Modbus
     * page lets the installer correct these live. */
    c->power_source       = SRC_MODBUS;
    c->power_slave        = 2;
    c->power_reg_voltage  = 0;
    c->power_reg_current  = 1;
    c->power_reg_power    = 2;
    c->power_reg_energy   = 4;
    c->power_coil_main    = 0;
    c->power_coil_solar   = 1;
    c->power_coil_battery = 2;
    c->power_coil_ups     = 3;

    c->temp_source       = SRC_I2C_SHT3X;
    c->temp_slave        = 1;
    c->temp_reg_indoor   = 100;
    c->temp_reg_humidity = 101;
    c->temp_reg_outdoor  = 102;
    c->temp_setpoint_c10 = 260;

    /* Access control. The face path starts switched off because a door should
     * never come up accepting a credential nobody has enrolled yet. */
    c->access_card_enabled   = true;
    c->access_face_enabled   = false;
    c->access_strike_ms      = 5000;
    c->access_max_failures   = 5;
    c->access_lockout_s      = 30;
    c->access_face_threshold = 80;
    c->access_snapshot       = true;
    c->access_beep           = true;

    c->clip_seconds       = 30;
    c->ring_delete_oldest = true;
    c->min_free_percent   = 10;

    for (int i = 0; i < APP_SWITCH_COUNT; i++) {
        app_switch_cfg_t *sw = &c->switches[i];
        strlcpy(sw->name, k_default_switch_names[i], APP_SWITCH_NAME_LEN);
        sw->bind       = SWITCH_BIND_MODBUS;
        sw->gpio       = -1;
        sw->slave_addr = 1;
        sw->coil_addr  = (uint16_t)i;
        sw->state      = false;
        sw->invert     = false;
    }
}

esp_err_t app_settings_init(void)
{
    if (s_loaded) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs an erase (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    load_defaults(&s_cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        app_settings_t stored;
        size_t len = sizeof(stored);
        esp_err_t rd = nvs_get_blob(h, NVS_KEY_BLOB, &stored, &len);
        nvs_close(h);

        if (rd == ESP_OK && len == sizeof(stored) && stored.version == APP_SETTINGS_VERSION) {
            s_cfg = stored;
            ESP_LOGI(TAG, "loaded settings v%lu", (unsigned long)s_cfg.version);
        } else if (rd == ESP_OK) {
            ESP_LOGW(TAG, "stored settings are v%lu/%uB, expected v%d/%uB -- using defaults",
                     (unsigned long)stored.version, (unsigned)len,
                     APP_SETTINGS_VERSION, (unsigned)sizeof(stored));
        }
    }

    s_loaded = true;
    return ESP_OK;
}

app_settings_t *app_settings(void)
{
    return &s_cfg;
}

esp_err_t app_settings_commit(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open");

    esp_err_t err = nvs_set_blob(h, NVS_KEY_BLOB, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        app_event_post(APP_EVT_SETTINGS_CHANGED, NULL, 0);
    } else {
        ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* --------------------------------------------------------------------------
 * Deferred commit
 * ------------------------------------------------------------------------ */
#define COMMIT_DEBOUNCE_US  (5 * 1000 * 1000)

static esp_timer_handle_t s_commit_timer;

static void commit_timer_cb(void *arg)
{
    (void)arg;
    app_settings_commit();
}

esp_err_t app_settings_commit_deferred(void)
{
    if (!s_commit_timer) {
        const esp_timer_create_args_t args = {
            .callback = commit_timer_cb,
            .name     = "cfg_commit",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_commit_timer), TAG, "timer");
    }

    /* Restarting the timer is what collapses a burst: only the last change in
     * a five-second window actually reaches flash. */
    esp_timer_stop(s_commit_timer);
    return esp_timer_start_once(s_commit_timer, COMMIT_DEBOUNCE_US);
}

esp_err_t app_settings_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    load_defaults(&s_cfg);
    return app_settings_commit();
}
