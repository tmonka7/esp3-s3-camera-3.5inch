#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"

#include "bsp_board.h"
#include "app_events.h"
#include "app_settings.h"
#include "app_net.h"
#include "mb_proto.h"
#include "mb_rtu.h"
#include "mb_tcp.h"
#include "svc_modbus.h"

static const char *TAG = "svc_modbus";

#define MB_LOCK_TOUT  pdMS_TO_TICKS(3000)

/* RTU and TCP are independent stacks with independent locks, so a slow RS485
 * poll never blocks a TCP request and vice versa. `s_active` only decides
 * which one an unqualified request goes out on. */
static SemaphoreHandle_t      s_rtu_lock;
static SemaphoreHandle_t      s_tcp_lock;
static svc_modbus_transport_t s_active = MB_TRANSPORT_NONE;
static app_link_state_t       s_link   = APP_LINK_DOWN;
static uint32_t               s_ok_count;
static uint32_t               s_fail_count;
static bool                   s_inited;

static void publish_link(app_link_state_t state)
{
    if (s_link != state) {
        s_link = state;
        app_event_post(APP_EVT_MODBUS_STATE, &s_link, sizeof(s_link));
    }
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */
esp_err_t svc_modbus_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_rtu_lock = xSemaphoreCreateMutex();
    s_tcp_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rtu_lock && s_tcp_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    s_inited = true;

    const app_settings_t *cfg = app_settings();

    /* Both transports come up at boot if configured. Nothing has to be torn
     * down to use the other one. */
    if (cfg->mb_rtu_enabled) {
        const esp_err_t err = mb_rtu_start(cfg->mb_rtu_baud, cfg->mb_rtu_parity);
        if (err == ESP_OK) {
            s_active = MB_TRANSPORT_RTU;
        } else {
            ESP_LOGW(TAG, "RTU did not start: %s", esp_err_to_name(err));
        }
    }

    if (cfg->mb_tcp_enabled) {
        if (mb_tcp_start(cfg->mb_tcp_host, cfg->mb_tcp_port) == ESP_OK &&
            s_active == MB_TRANSPORT_NONE) {
            s_active = MB_TRANSPORT_TCP;
        }
    }

    if (s_active == MB_TRANSPORT_NONE) {
        ESP_LOGI(TAG, "no Modbus transport enabled");
        publish_link(APP_LINK_DOWN);
    } else {
        publish_link(APP_LINK_CONNECTING);
    }
    return ESP_OK;
}

esp_err_t svc_modbus_deinit(void)
{
    mb_rtu_stop();
    mb_tcp_stop();
    s_active = MB_TRANSPORT_NONE;
    publish_link(APP_LINK_DOWN);
    return ESP_OK;
}

esp_err_t svc_modbus_set_transport(svc_modbus_transport_t t)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    const app_settings_t *cfg = app_settings();

    switch (t) {
    case MB_TRANSPORT_RTU:
        if (!mb_rtu_is_up()) {
            ESP_RETURN_ON_ERROR(mb_rtu_start(cfg->mb_rtu_baud, cfg->mb_rtu_parity),
                                TAG, "rtu start");
        }
        break;

    case MB_TRANSPORT_TCP:
        ESP_RETURN_ON_FALSE(app_net_is_connected(), ESP_ERR_INVALID_STATE,
                            TAG, "Wi-Fi is down");
        if (!mb_tcp_is_up()) {
            ESP_RETURN_ON_ERROR(mb_tcp_start(cfg->mb_tcp_host, cfg->mb_tcp_port),
                                TAG, "tcp start");
        }
        break;

    case MB_TRANSPORT_NONE:
    default:
        break;
    }

    s_active = t;
    publish_link(t == MB_TRANSPORT_NONE ? APP_LINK_DOWN : APP_LINK_CONNECTING);
    return ESP_OK;
}

svc_modbus_transport_t svc_modbus_get_transport(void)
{
    return s_active;
}

app_link_state_t svc_modbus_link_state(void)
{
    return s_link;
}

/* --------------------------------------------------------------------------
 * Transactions
 * ------------------------------------------------------------------------ */
typedef esp_err_t (*mb_transact_fn)(uint8_t, uint8_t, uint16_t, uint16_t,
                                    const uint16_t *, void *);

static esp_err_t transact(uint8_t slave, uint8_t fn, uint16_t addr,
                          uint16_t count, const uint16_t *wr, void *out)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    SemaphoreHandle_t lock;
    mb_transact_fn    fp;

    switch (s_active) {
    case MB_TRANSPORT_RTU:
        lock = s_rtu_lock;
        fp   = mb_rtu_transact;
        break;
    case MB_TRANSPORT_TCP:
        lock = s_tcp_lock;
        fp   = mb_tcp_transact;
        break;
    default:
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(lock, MB_LOCK_TOUT) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t err = fp(slave, fn, addr, count, wr, out);

    if (err == ESP_OK) {
        s_ok_count++;
        publish_link(APP_LINK_UP);
    } else {
        s_fail_count++;
        ESP_LOGW(TAG, "slave %u fn %u @%u x%u -> %s",
                 slave, fn, addr, count, esp_err_to_name(err));
        /* A single timeout is normal on a shared RS485 segment; only report
         * the link as broken once failures clearly dominate. */
        if (s_fail_count > s_ok_count + 8) {
            publish_link(APP_LINK_ERROR);
        }
    }

    xSemaphoreGive(lock);
    return err;
}

esp_err_t svc_modbus_read_regs(uint8_t slave, svc_modbus_fn_t fn,
                               uint16_t addr, uint16_t count, uint16_t *out)
{
    ESP_RETURN_ON_FALSE(out && count && count <= SVC_MODBUS_MAX_REGS,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(fn == MB_FN_READ_HOLDING || fn == MB_FN_READ_INPUT,
                        ESP_ERR_INVALID_ARG, TAG, "not a register read");

    return transact(slave, (uint8_t)fn, addr, count, NULL, out);
}

esp_err_t svc_modbus_read_bits(uint8_t slave, svc_modbus_fn_t fn,
                               uint16_t addr, uint16_t count, uint8_t *out)
{
    ESP_RETURN_ON_FALSE(out && count, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(fn == MB_FN_READ_COILS || fn == MB_FN_READ_DISCRETE,
                        ESP_ERR_INVALID_ARG, TAG, "not a bit read");

    return transact(slave, (uint8_t)fn, addr, count, NULL, out);
}

esp_err_t svc_modbus_write_reg(uint8_t slave, uint16_t addr, uint16_t value)
{
    const uint16_t v = value;
    return transact(slave, MB_FN_WRITE_SINGLE_REG, addr, 1, &v, NULL);
}

esp_err_t svc_modbus_write_regs(uint8_t slave, uint16_t addr,
                                uint16_t count, const uint16_t *values)
{
    ESP_RETURN_ON_FALSE(values && count && count <= SVC_MODBUS_MAX_REGS,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");

    return transact(slave, MB_FN_WRITE_MULTI_REGS, addr, count, values, NULL);
}

esp_err_t svc_modbus_write_coil(uint8_t slave, uint16_t addr, bool on)
{
    const uint16_t v = on ? 1 : 0;
    return transact(slave, MB_FN_WRITE_SINGLE_COIL, addr, 1, &v, NULL);
}

esp_err_t svc_modbus_ui_request(uint8_t slave, svc_modbus_fn_t fn,
                                uint16_t addr, uint16_t count,
                                const uint16_t *write_values)
{
    svc_modbus_result_t res = {
        .slave    = slave,
        .function = (uint8_t)fn,
        .address  = addr,
        .count    = count,
    };

    if (count > SVC_MODBUS_MAX_REGS) {
        count     = SVC_MODBUS_MAX_REGS;
        res.count = count;
    }

    esp_err_t err;
    switch (fn) {
    case MB_FN_READ_HOLDING:
    case MB_FN_READ_INPUT:
        err = svc_modbus_read_regs(slave, fn, addr, count, res.values);
        break;

    case MB_FN_READ_COILS:
    case MB_FN_READ_DISCRETE: {
        uint8_t bits[(SVC_MODBUS_MAX_REGS + 7) / 8] = {0};
        err = svc_modbus_read_bits(slave, fn, addr, count, bits);
        /* Expand to one value per point so the UI table stays uniform. */
        for (uint16_t i = 0; i < count; i++) {
            res.values[i] = (bits[i / 8] >> (i % 8)) & 0x01;
        }
        break;
    }

    case MB_FN_WRITE_SINGLE_REG:
        err = write_values ? svc_modbus_write_reg(slave, addr, write_values[0])
                           : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK && write_values) {
            res.values[0] = write_values[0];
            res.count     = 1;
        }
        break;

    case MB_FN_WRITE_MULTI_REGS:
        err = write_values ? svc_modbus_write_regs(slave, addr, count, write_values)
                           : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK && write_values) {
            memcpy(res.values, write_values, count * sizeof(uint16_t));
        }
        break;

    case MB_FN_WRITE_SINGLE_COIL:
        err = write_values ? svc_modbus_write_coil(slave, addr, write_values[0] != 0)
                           : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK && write_values) {
            res.values[0] = write_values[0] ? 1 : 0;
            res.count     = 1;
        }
        break;

    default:
        err = ESP_ERR_NOT_SUPPORTED;
        break;
    }

    res.ok  = (err == ESP_OK);
    res.err = (int)err;
    app_event_post(APP_EVT_MODBUS_RESULT, &res, sizeof(res));
    return err;
}

void svc_modbus_get_stats(uint32_t *ok, uint32_t *failed)
{
    if (ok)     *ok     = s_ok_count;
    if (failed) *failed = s_fail_count;
}
