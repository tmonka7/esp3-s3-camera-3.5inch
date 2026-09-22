#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"

#include "bsp_io_expander.h"
#include "app_events.h"
#include "app_settings.h"
#include "svc_modbus.h"
#include "svc_switch.h"

static const char *TAG = "svc_switch";

/* The last entry is the "all lights" scene rather than a room. */
#define SCENE_INDEX  (APP_SWITCH_COUNT - 1)

/* How often the coil states are re-read from the bus. */
#define SWITCH_POLL_MS  5000

static bool s_inited;

static void publish(uint8_t index, bool state, bool ok)
{
    const switch_update_t u = { .index = index, .state = state, .ok = ok };
    app_event_post(APP_EVT_SWITCH_UPDATE, &u, sizeof(u));
}

/** Drives the physical output for one switch, ignoring persistence. */
static esp_err_t drive_output(const app_switch_cfg_t *sw, bool on)
{
    const bool level = sw->invert ? !on : on;

    switch (sw->bind) {
    case SWITCH_BIND_GPIO:
        if (sw->gpio < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        return gpio_set_level((gpio_num_t)sw->gpio, level ? 1 : 0);

    case SWITCH_BIND_IOEXP:
        return bsp_io_expander_set(sw->gpio, level);

    case SWITCH_BIND_MODBUS:
        return svc_modbus_write_coil(sw->slave_addr, sw->coil_addr, level);

    case SWITCH_BIND_NONE:
    default:
        return ESP_OK;          /* UI-only switch */
    }
}

static esp_err_t configure_gpio(const app_switch_cfg_t *sw)
{
    if (sw->bind != SWITCH_BIND_GPIO || sw->gpio < 0) {
        return ESP_OK;
    }

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << sw->gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

/* Coils can be driven by other controllers on the same segment, so the
 * panel re-reads them periodically instead of trusting its own last write.
 * This has to live on its own task: each read blocks for up to the Modbus
 * response timeout, which the UI task cannot afford. */
static void poll_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SWITCH_POLL_MS));
        svc_switch_refresh_from_bus();
    }
}

esp_err_t svc_switch_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    app_settings_t *cfg = app_settings();

    for (uint8_t i = 0; i < APP_SWITCH_COUNT; i++) {
        app_switch_cfg_t *sw = &cfg->switches[i];
        ESP_RETURN_ON_ERROR(configure_gpio(sw), TAG, "gpio cfg %u", i);

        /* Restore the saved state so a power cut does not silently turn the
         * house off. The scene entry is derived, not driven. */
        if (i != SCENE_INDEX) {
            const esp_err_t err = drive_output(sw, sw->state);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "restore %s failed: %s", sw->name, esp_err_to_name(err));
            }
        }
    }

    s_inited = true;

    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(poll_task, "switches", 3584, NULL, 3,
                                                NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "poll task");

    ESP_LOGI(TAG, "%d switches ready", APP_SWITCH_COUNT);
    return ESP_OK;
}

esp_err_t svc_switch_set(uint8_t index, bool on)
{
    ESP_RETURN_ON_FALSE(index < APP_SWITCH_COUNT, ESP_ERR_INVALID_ARG, TAG, "range");

    if (index == SCENE_INDEX) {
        return svc_switch_set_all(on);
    }

    app_switch_cfg_t *sw  = &app_settings()->switches[index];
    const esp_err_t   err = drive_output(sw, on);

    /* Track the requested state even when the write failed: the UI shows the
     * intent plus an error badge, and a later retry can re-drive it. */
    sw->state = on;
    app_settings_commit_deferred();
    publish(index, on, err == ESP_OK);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s -> %s failed: %s", sw->name, on ? "ON" : "OFF",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t svc_switch_toggle(uint8_t index)
{
    ESP_RETURN_ON_FALSE(index < APP_SWITCH_COUNT, ESP_ERR_INVALID_ARG, TAG, "range");
    return svc_switch_set(index, !app_settings()->switches[index].state);
}

bool svc_switch_get(uint8_t index)
{
    return index < APP_SWITCH_COUNT && app_settings()->switches[index].state;
}

esp_err_t svc_switch_set_all(bool on)
{
    app_settings_t *cfg     = app_settings();
    esp_err_t       overall = ESP_OK;

    for (uint8_t i = 0; i < SCENE_INDEX; i++) {
        app_switch_cfg_t *sw = &cfg->switches[i];
        if (sw->bind == SWITCH_BIND_NONE) {
            continue;
        }

        const esp_err_t err = drive_output(sw, on);
        if (err != ESP_OK && overall == ESP_OK) {
            overall = err;
        }
        sw->state = on;
        publish(i, on, err == ESP_OK);
    }

    cfg->switches[SCENE_INDEX].state = on;
    publish(SCENE_INDEX, on, overall == ESP_OK);

    /* One commit for the whole scene rather than ten. */
    app_settings_commit_deferred();
    return overall;
}

const char *svc_switch_name(uint8_t index)
{
    return index < APP_SWITCH_COUNT ? app_settings()->switches[index].name : "";
}

esp_err_t svc_switch_refresh_from_bus(void)
{
    if (svc_modbus_get_transport() == MB_TRANSPORT_NONE) {
        return ESP_OK;
    }

    app_settings_t *cfg     = app_settings();
    bool            changed = false;

    for (uint8_t i = 0; i < SCENE_INDEX; i++) {
        app_switch_cfg_t *sw = &cfg->switches[i];
        if (sw->bind != SWITCH_BIND_MODBUS) {
            continue;
        }

        uint8_t bits = 0;
        if (svc_modbus_read_bits(sw->slave_addr, MB_FN_READ_COILS,
                                 sw->coil_addr, 1, &bits) != ESP_OK) {
            continue;   /* leave the cached state alone on a read failure */
        }

        bool state = (bits & 0x01) != 0;
        if (sw->invert) {
            state = !state;
        }

        if (state != sw->state) {
            sw->state = state;
            changed   = true;
            publish(i, state, true);
        }
    }

    if (changed) {
        app_settings_commit_deferred();
    }
    return ESP_OK;
}
