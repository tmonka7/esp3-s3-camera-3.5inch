#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

#include "bsp_board.h"
#include "bsp_i2c.h"
#include "app_events.h"
#include "app_settings.h"
#include "svc_modbus.h"
#include "svc_power.h"

static const char *TAG = "svc_power";

#define POLL_PERIOD_MS      2000
/* One chart point per 15 minutes -- 96 points span a day. */
#define HISTORY_PERIOD_MS   (15 * 60 * 1000)

static power_status_t s_status;
static uint32_t       s_history[SVC_POWER_HISTORY];
static size_t         s_hist_head;
static size_t         s_hist_count;
static bool           s_running;

static void history_push(uint32_t watts)
{
    s_history[s_hist_head] = watts;
    s_hist_head = (s_hist_head + 1) % SVC_POWER_HISTORY;
    if (s_hist_count < SVC_POWER_HISTORY) {
        s_hist_count++;
    }
}

size_t svc_power_history(uint32_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }

    /* Copy the newest `n` points, oldest first. */
    const size_t n     = (s_hist_count < max) ? s_hist_count : max;
    const size_t start = (s_hist_head + SVC_POWER_HISTORY - n) % SVC_POWER_HISTORY;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_history[(start + i) % SVC_POWER_HISTORY];
    }
    return n;
}

/* --------------------------------------------------------------------------
 * AXP2101 battery gauge
 *
 * Only the fuel-gauge percentage is read here; the PMU's rails are managed by
 * the hardware itself and must not be reconfigured from the application.
 * ------------------------------------------------------------------------ */
#define AXP2101_REG_BATT_PERCENT  0xA4

static uint8_t read_battery_percent(void)
{
    uint8_t pct = 0;
    if (bsp_i2c_read_reg(BSP_I2C_ADDR_AXP2101, AXP2101_REG_BATT_PERCENT, &pct) != ESP_OK) {
        return 0;
    }
    return pct <= 100 ? pct : 0;
}

/* --------------------------------------------------------------------------
 * Meter polling
 * ------------------------------------------------------------------------ */
static bool read_meter(power_status_t *out)
{
    const app_settings_t *cfg = app_settings();

    if (cfg->power_source != SRC_MODBUS ||
        svc_modbus_get_transport() == MB_TRANSPORT_NONE) {
        return false;
    }

    /* The four quantities are rarely contiguous across meter models, so each
     * is read on its own rather than assuming one block. */
    struct {
        uint16_t  addr;
        uint16_t  count;
        uint32_t *dst32;
        uint16_t *dst16;
    } fields[] = {
        { cfg->power_reg_voltage, 1, NULL,             &out->voltage_v10  },
        { cfg->power_reg_current, 1, NULL,             &out->current_a100 },
        { cfg->power_reg_power,   1, &out->power_w,    NULL               },
        { cfg->power_reg_energy,  2, &out->energy_wh,  NULL               },
    };

    bool any = false;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        uint16_t regs[2] = {0, 0};
        if (svc_modbus_read_regs(cfg->power_slave, MB_FN_READ_HOLDING,
                                 fields[i].addr, fields[i].count, regs) != ESP_OK) {
            continue;
        }

        if (fields[i].dst16) {
            *fields[i].dst16 = regs[0];
        } else {
            /* Two-register values are big-endian word order, as almost every
             * meter reports 32-bit energy counters. */
            *fields[i].dst32 = (fields[i].count == 2)
                             ? (((uint32_t)regs[0] << 16) | regs[1])
                             : regs[0];
        }
        any = true;
    }
    return any;
}

static void read_rails(power_status_t *out)
{
    const app_settings_t *cfg = app_settings();
    if (svc_modbus_get_transport() == MB_TRANSPORT_NONE) {
        return;
    }

    const uint16_t coils[POWER_RAIL_COUNT] = {
        cfg->power_coil_main,
        cfg->power_coil_solar,
        cfg->power_coil_battery,
        cfg->power_coil_ups,
    };

    for (int i = 0; i < POWER_RAIL_COUNT; i++) {
        uint8_t bits = 0;
        if (svc_modbus_read_bits(cfg->power_slave, MB_FN_READ_COILS,
                                 coils[i], 1, &bits) == ESP_OK) {
            out->rail[i] = (bits & 0x01) != 0;
        }
    }
}

static void poll_task(void *arg)
{
    (void)arg;

    TickType_t last_history = xTaskGetTickCount();

    while (s_running) {
        power_status_t next = s_status;

        next.valid           = read_meter(&next);
        next.battery_percent = read_battery_percent();
        read_rails(&next);

        s_status = next;
        app_event_post(APP_EVT_POWER_UPDATE, &s_status, sizeof(s_status));

        if ((xTaskGetTickCount() - last_history) >= pdMS_TO_TICKS(HISTORY_PERIOD_MS)) {
            history_push(s_status.power_w);
            last_history = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }

    vTaskDelete(NULL);
}

esp_err_t svc_power_init(void)
{
    if (s_running) {
        return ESP_OK;
    }

    s_running = true;
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(poll_task, "power", 4096, NULL, 4,
                                                NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");

    ESP_LOGI(TAG, "power monitor started");
    return ESP_OK;
}

void svc_power_get(power_status_t *out)
{
    if (out) {
        *out = s_status;
    }
}

esp_err_t svc_power_set_rail(power_rail_t rail, bool on)
{
    ESP_RETURN_ON_FALSE(rail < POWER_RAIL_COUNT, ESP_ERR_INVALID_ARG, TAG, "range");

    const app_settings_t *cfg = app_settings();
    const uint16_t coils[POWER_RAIL_COUNT] = {
        cfg->power_coil_main,
        cfg->power_coil_solar,
        cfg->power_coil_battery,
        cfg->power_coil_ups,
    };

    const esp_err_t err = svc_modbus_write_coil(cfg->power_slave, coils[rail], on);
    if (err == ESP_OK) {
        s_status.rail[rail] = on;
        app_event_post(APP_EVT_POWER_UPDATE, &s_status, sizeof(s_status));
    } else {
        ESP_LOGW(TAG, "rail %d -> %s failed: %s", (int)rail, on ? "ON" : "OFF",
                 esp_err_to_name(err));
    }
    return err;
}
