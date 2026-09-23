/* Home Automation panel -- Waveshare ESP32-S3-Touch-LCD-3.5-C
 *
 * Boot order matters here:
 *   settings -> display -> splash -> everything else.
 * The panel is showing progress within about a second, and every service
 * that follows can fail without taking the UI down with it. A missing TF
 * card, an absent RS485 segment or an unplugged camera each disable one
 * page rather than the product.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include "bsp_board.h"
#include "esp_camera.h"
#include "bsp_camera.h"
#include "bsp_display.h"
#include "bsp_storage.h"

#include "app_events.h"
#include "app_net.h"
#include "app_settings.h"
#include "app_time.h"

#include "svc_detect.h"
#include "svc_media.h"
#include "media_player.h"
#include "svc_modbus.h"
#include "svc_notify.h"
#include "svc_power.h"
#include "svc_access.h"
#include "svc_switch.h"
#include "svc_temp.h"
#include "svc_uart.h"

#include "ui.h"

static const char *TAG = "main";

/** Runs one start-up step, updating the splash and never aborting the boot. */
static void step(int percent, const char *message, esp_err_t (*fn)(void))
{
    ui_splash_progress(percent, message);

    const esp_err_t err = fn();
    if (err != ESP_OK) {
        /* Deliberately not fatal: the affected page reports its own state. */
        ESP_LOGW(TAG, "%s: %s", message, esp_err_to_name(err));
    }
}

/* --------------------------------------------------------------------------
 * Auto sleep
 *
 * Dims the backlight after the configured idle period and restores it on the
 * next touch. LVGL already tracks input activity, so no extra plumbing is
 * needed -- and the camera pipeline keeps running, so detection and
 * recording continue while the screen is dark.
 * ------------------------------------------------------------------------ */
static void sleep_task(void *arg)
{
    (void)arg;

    bool dimmed = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        const uint16_t idle_min = app_settings()->auto_sleep_min;
        if (idle_min == 0) {
            if (dimmed) {
                bsp_display_set_brightness(app_settings()->brightness);
                dimmed = false;
            }
            continue;
        }

        uint32_t idle_ms = 0;
        if (bsp_display_lock(0)) {
            idle_ms = lv_disp_get_inactive_time(NULL);
            bsp_display_unlock();
        }

        const bool should_dim = idle_ms > (uint32_t)idle_min * 60u * 1000u;
        if (should_dim && !dimmed) {
            ESP_LOGI(TAG, "idle for %u min -- dimming", idle_min);
            bsp_display_set_brightness(0);
            dimmed = true;
        } else if (!should_dim && dimmed) {
            bsp_display_set_brightness(app_settings()->brightness);
            dimmed = false;
        }
    }
}

/* --------------------------------------------------------------------------
 * Service wrappers
 *
 * A few services need an argument or a decision that `step()` cannot carry,
 * so they get a thin zero-argument wrapper here.
 * ------------------------------------------------------------------------ */
static esp_err_t start_uart(void)
{
    ESP_RETURN_ON_ERROR(svc_uart_init(), TAG, "uart init");
    /* Opening the port at boot means the page shows traffic straight away
     * rather than waiting for someone to press Open. */
    return svc_uart_open(NULL);
}

static esp_err_t start_climate(void)
{
    const esp_err_t temp  = svc_temp_init();
    const esp_err_t power = svc_power_init();
    return (temp != ESP_OK) ? temp : power;
}

static esp_err_t start_camera(void)
{
    ESP_RETURN_ON_ERROR(bsp_camera_init(), TAG, "camera");

    /* The BSP brings the sensor up with safe defaults; the saved preferences
     * are applied here, where app_core is in scope. */
    const app_settings_t *cfg = app_settings();
    bsp_camera_set_framesize((framesize_t)cfg->cam_framesize);
    bsp_camera_set_quality(cfg->cam_quality);
    bsp_camera_set_flip(cfg->cam_hmirror, cfg->cam_vflip);
    return ESP_OK;
}

static esp_err_t start_media(void)
{
    ESP_RETURN_ON_ERROR(svc_media_init(), TAG, "media");
    return media_player_init();
}

static esp_err_t mount_storage(void)
{
    bsp_storage_mount_assets();
    return bsp_storage_mount();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Home Automation panel starting");

    /* The event bus has to exist before anything can publish to it. */
    ESP_ERROR_CHECK(app_events_init());
    ESP_ERROR_CHECK(app_settings_init());

    /* Display first, so the rest of the boot is visible. */
    ESP_ERROR_CHECK(bsp_display_init());
    ESP_ERROR_CHECK(ui_init());
    bsp_display_set_brightness(app_settings()->brightness);

    step(10, "Mounting TF card...",     mount_storage);
    step(20, "Setting the clock...",    app_time_init);
    step(30, "Connecting Wi-Fi...",     app_net_init);
    step(45, "Starting camera...",      start_camera);
    step(58, "Starting media...",       start_media);
    step(66, "Preparing alerts...",     svc_notify_init);
    step(72, "Starting detection...",   svc_detect_init);
    step(80, "Opening serial port...",  start_uart);
    step(88, "Starting Modbus...",      svc_modbus_init);
    step(93, "Restoring switches...",   svc_switch_init);
    step(95, "Starting access control...", svc_access_init);
    step(97, "Reading meters...",       start_climate);

    ui_splash_progress(100, "Ready");
    vTaskDelay(pdMS_TO_TICKS(400));

    xTaskCreatePinnedToCore(sleep_task, "autosleep", 3072, NULL, 2, NULL, 0);

    ui_show(UI_SCREEN_HOME);

    ESP_LOGI(TAG, "up: %u KB internal / %u KB PSRAM free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
