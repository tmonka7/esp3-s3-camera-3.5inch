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
#include "svc_temp.h"

static const char *TAG = "svc_temp";

#define POLL_PERIOD_MS   5000
#define SETPOINT_MIN_C10  50    /*  5.0 C */
#define SETPOINT_MAX_C10 350    /* 35.0 C */

static temp_status_t s_status;
static bool          s_running;
static bool          s_sht3x_present;

static const char *k_room_names[SVC_TEMP_ROOMS] = {
    "Living Room", "Bedroom", "Kitchen", "Study",
};

/* --------------------------------------------------------------------------
 * SHT3x (optional, address 0x44)
 * ------------------------------------------------------------------------ */
#define SHT3X_CMD_SINGLE_HIGH_HI  0x2C
#define SHT3X_CMD_SINGLE_HIGH_LO  0x06

static uint8_t sht3x_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t sht3x_read(int16_t *temp_c10, uint8_t *rh_pct)
{
    const uint8_t cmd[2] = { SHT3X_CMD_SINGLE_HIGH_HI, SHT3X_CMD_SINGLE_HIGH_LO };
    ESP_RETURN_ON_ERROR(bsp_i2c_write(BSP_I2C_ADDR_SHT3X, cmd, sizeof(cmd)),
                        TAG, "sht3x cmd");

    /* A high-repeatability conversion needs ~15 ms. */
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t r[6] = {0};
    ESP_RETURN_ON_ERROR(bsp_i2c_read(BSP_I2C_ADDR_SHT3X, r, sizeof(r)), TAG, "sht3x read");

    if (sht3x_crc(&r[0], 2) != r[2] || sht3x_crc(&r[3], 2) != r[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_t = (uint16_t)((r[0] << 8) | r[1]);
    const uint16_t raw_h = (uint16_t)((r[3] << 8) | r[4]);

    /* T = -45 + 175 * raw / 65535, kept in tenths without floating point. */
    *temp_c10 = (int16_t)(((int32_t)raw_t * 1750) / 65535 - 450);
    *rh_pct   = (uint8_t)(((uint32_t)raw_h * 100) / 65535);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Modbus fallback
 * ------------------------------------------------------------------------ */
static bool read_reg_c10(uint16_t addr, int16_t *out)
{
    const app_settings_t *cfg = app_settings();
    if (svc_modbus_get_transport() == MB_TRANSPORT_NONE) {
        return false;
    }

    uint16_t raw = 0;
    if (svc_modbus_read_regs(cfg->temp_slave, MB_FN_READ_HOLDING, addr, 1, &raw) != ESP_OK) {
        return false;
    }
    /* Registers are signed tenths of a degree, the usual convention for
     * HVAC gateways. */
    *out = (int16_t)raw;
    return true;
}

static void poll_once(void)
{
    const app_settings_t *cfg  = app_settings();
    temp_status_t         next = s_status;

    next.setpoint_c10 = cfg->temp_setpoint_c10;
    next.indoor_valid = false;

    if (cfg->temp_source == SRC_I2C_SHT3X && s_sht3x_present) {
        int16_t t = 0;
        uint8_t h = 0;
        if (sht3x_read(&t, &h) == ESP_OK) {
            next.indoor_c10   = t;
            next.humidity_pct = h;
            next.indoor_valid = true;
        }
    } else if (cfg->temp_source == SRC_MODBUS) {
        int16_t t = 0;
        if (read_reg_c10(cfg->temp_reg_indoor, &t)) {
            next.indoor_c10   = t;
            next.indoor_valid = true;
        }
        int16_t h = 0;
        if (read_reg_c10(cfg->temp_reg_humidity, &h) && h >= 0 && h <= 1000) {
            next.humidity_pct = (uint8_t)(h / 10);
        }
    }

    int16_t outdoor = 0;
    next.outdoor_valid = read_reg_c10(cfg->temp_reg_outdoor, &outdoor);
    if (next.outdoor_valid) {
        next.outdoor_c10 = outdoor;
    }

    /* Rooms live in consecutive registers above the indoor address. With a
     * single local sensor the first room simply mirrors it. */
    for (int i = 0; i < SVC_TEMP_ROOMS; i++) {
        int16_t v = 0;
        if (read_reg_c10((uint16_t)(cfg->temp_reg_indoor + 10 + i), &v)) {
            next.room_c10[i]   = v;
            next.room_valid[i] = true;
        } else if (i == 0 && next.indoor_valid) {
            next.room_c10[0]   = next.indoor_c10;
            next.room_valid[0] = true;
        }
    }

    s_status = next;
    app_event_post(APP_EVT_TEMP_UPDATE, &s_status, sizeof(s_status));
}

static void poll_task(void *arg)
{
    (void)arg;
    while (s_running) {
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

esp_err_t svc_temp_init(void)
{
    if (s_running) {
        return ESP_OK;
    }

    s_sht3x_present = bsp_i2c_probe(BSP_I2C_ADDR_SHT3X);
    if (!s_sht3x_present && app_settings()->temp_source == SRC_I2C_SHT3X) {
        ESP_LOGW(TAG, "no SHT3x fitted -- indoor readings need a Modbus source");
    }

    s_status.setpoint_c10 = app_settings()->temp_setpoint_c10;
    s_running = true;

    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(poll_task, "temp", 4096, NULL, 3,
                                                NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void svc_temp_get(temp_status_t *out)
{
    if (out) {
        *out = s_status;
    }
}

esp_err_t svc_temp_set_setpoint(int16_t c10)
{
    if (c10 < SETPOINT_MIN_C10) c10 = SETPOINT_MIN_C10;
    if (c10 > SETPOINT_MAX_C10) c10 = SETPOINT_MAX_C10;

    app_settings_t *cfg   = app_settings();
    cfg->temp_setpoint_c10 = c10;
    s_status.setpoint_c10  = c10;
    app_settings_commit_deferred();

    /* Push the new setpoint to the HVAC controller when one is reachable. */
    if (cfg->temp_source == SRC_MODBUS &&
        svc_modbus_get_transport() != MB_TRANSPORT_NONE) {
        const esp_err_t err = svc_modbus_write_reg(cfg->temp_slave,
                                                   (uint16_t)(cfg->temp_reg_indoor + 1),
                                                   (uint16_t)c10);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "setpoint write failed: %s", esp_err_to_name(err));
        }
    }

    app_event_post(APP_EVT_TEMP_UPDATE, &s_status, sizeof(s_status));
    return ESP_OK;
}

esp_err_t svc_temp_nudge_setpoint(int16_t delta_c10)
{
    return svc_temp_set_setpoint((int16_t)(s_status.setpoint_c10 + delta_c10));
}

const char *svc_temp_room_name(int index)
{
    return (index >= 0 && index < SVC_TEMP_ROOMS) ? k_room_names[index] : "";
}
