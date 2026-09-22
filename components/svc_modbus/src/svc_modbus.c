#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "mbcontroller.h"

#include "bsp_board.h"
#include "app_events.h"
#include "app_settings.h"
#include "app_net.h"
#include "svc_modbus.h"

static const char *TAG = "svc_modbus";

#define MB_LOCK_TOUT          pdMS_TO_TICKS(3000)

static void                  *s_master;
static svc_modbus_transport_t s_transport = MB_TRANSPORT_NONE;
static app_link_state_t       s_link      = APP_LINK_DOWN;
static SemaphoreHandle_t      s_lock;
static uint32_t               s_ok_count;
static uint32_t               s_fail_count;

/* esp-modbus keeps the TCP slave list as an array of address strings. */
static char  s_tcp_addr[40];
static char *s_tcp_addr_table[2];

static void publish_link(app_link_state_t state)
{
    if (s_link != state) {
        s_link = state;
        app_event_post(APP_EVT_MODBUS_STATE, &s_link, sizeof(s_link));
    }
}

/* --------------------------------------------------------------------------
 * Stack lifecycle
 * ------------------------------------------------------------------------ */
static esp_err_t start_rtu(void)
{
    ESP_RETURN_ON_ERROR(mbc_master_init(MB_PORT_SERIAL_MASTER, &s_master),
                        TAG, "rtu init");

    const app_settings_t *cfg = app_settings();
    mb_communication_info_t comm = {
        .port      = BSP_RS485_UART_NUM,
        .mode      = MB_MODE_RTU,
        .baudrate  = cfg->mb_rtu_baud,
        .parity    = (cfg->mb_rtu_parity == 1) ? UART_PARITY_ODD
                   : (cfg->mb_rtu_parity == 2) ? UART_PARITY_EVEN
                                               : UART_PARITY_DISABLE,
    };
    ESP_RETURN_ON_ERROR(mbc_master_setup((void *)&comm), TAG, "rtu setup");

    /* Pins must be assigned after setup() installed the UART driver and
     * before start(). DE/RE is driven by the UART in RS485 half-duplex mode
     * when a direction pin exists. */
    ESP_RETURN_ON_ERROR(uart_set_pin(BSP_RS485_UART_NUM,
                                     BSP_RS485_PIN_TX, BSP_RS485_PIN_RX,
                                     BSP_RS485_PIN_DE >= 0 ? BSP_RS485_PIN_DE
                                                           : UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "rs485 pins");

    ESP_RETURN_ON_ERROR(mbc_master_start(), TAG, "rtu start");
    ESP_RETURN_ON_ERROR(uart_set_mode(BSP_RS485_UART_NUM,
                                      BSP_RS485_PIN_DE >= 0 ? UART_MODE_RS485_HALF_DUPLEX
                                                            : UART_MODE_UART),
                        TAG, "rs485 mode");

    ESP_LOGI(TAG, "Modbus-RTU master up on UART%d @ %lu",
             BSP_RS485_UART_NUM, (unsigned long)cfg->mb_rtu_baud);
    return ESP_OK;
}

static esp_err_t start_tcp(void)
{
    ESP_RETURN_ON_FALSE(app_net_is_connected(), ESP_ERR_INVALID_STATE,
                        TAG, "Wi-Fi is down");

    ESP_RETURN_ON_ERROR(mbc_master_init(MB_PORT_TCP_MASTER, &s_master),
                        TAG, "tcp init");

    const app_settings_t *cfg = app_settings();
    strlcpy(s_tcp_addr, cfg->mb_tcp_host, sizeof(s_tcp_addr));
    s_tcp_addr_table[0] = s_tcp_addr;
    s_tcp_addr_table[1] = NULL;

    mb_communication_info_t comm = {
        .ip_port      = cfg->mb_tcp_port,
        .ip_addr_type = MB_IPV4,
        .ip_mode      = MB_MODE_TCP,
        .ip_addr      = (void *)s_tcp_addr_table,
        .ip_netif_ptr = app_net_netif(),
    };
    ESP_RETURN_ON_ERROR(mbc_master_setup((void *)&comm), TAG, "tcp setup");
    ESP_RETURN_ON_ERROR(mbc_master_start(), TAG, "tcp start");

    ESP_LOGI(TAG, "Modbus-TCP master up against %s:%u", s_tcp_addr, cfg->mb_tcp_port);
    return ESP_OK;
}

static void stop_stack(void)
{
    if (s_transport == MB_TRANSPORT_NONE) {
        return;
    }
    mbc_master_destroy();
    s_master    = NULL;
    s_transport = MB_TRANSPORT_NONE;
    publish_link(APP_LINK_DOWN);
}

esp_err_t svc_modbus_set_transport(svc_modbus_transport_t t)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    if (xSemaphoreTake(s_lock, MB_LOCK_TOUT) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    if (t != s_transport) {
        stop_stack();
        publish_link(APP_LINK_CONNECTING);

        switch (t) {
        case MB_TRANSPORT_RTU:  err = start_rtu(); break;
        case MB_TRANSPORT_TCP:  err = start_tcp(); break;
        case MB_TRANSPORT_NONE: err = ESP_OK;      break;
        }

        if (err == ESP_OK) {
            s_transport = t;
            publish_link(t == MB_TRANSPORT_NONE ? APP_LINK_DOWN : APP_LINK_UP);
        } else {
            ESP_LOGE(TAG, "transport %d failed: %s", (int)t, esp_err_to_name(err));
            /* A half-built stack would leak its UART/socket on the next try. */
            mbc_master_destroy();
            s_master = NULL;
            publish_link(APP_LINK_ERROR);
        }
    }

    xSemaphoreGive(s_lock);
    return err;
}

svc_modbus_transport_t svc_modbus_get_transport(void)
{
    return s_transport;
}

app_link_state_t svc_modbus_link_state(void)
{
    return s_link;
}

esp_err_t svc_modbus_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    const app_settings_t *cfg = app_settings();
    if (cfg->mb_rtu_enabled) {
        return svc_modbus_set_transport(MB_TRANSPORT_RTU);
    }
    if (cfg->mb_tcp_enabled) {
        return svc_modbus_set_transport(MB_TRANSPORT_TCP);
    }

    ESP_LOGI(TAG, "no Modbus transport enabled");
    return ESP_OK;
}

esp_err_t svc_modbus_deinit(void)
{
    if (!s_lock) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, MB_LOCK_TOUT);
    stop_stack();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Transactions
 * ------------------------------------------------------------------------ */
static esp_err_t transact(uint8_t slave, uint8_t fn, uint16_t addr,
                          uint16_t count, void *data)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(s_transport != MB_TRANSPORT_NONE, ESP_ERR_INVALID_STATE,
                        TAG, "no transport");

    if (xSemaphoreTake(s_lock, MB_LOCK_TOUT) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    mb_param_request_t req = {
        .slave_addr = slave,
        .command    = fn,
        .reg_start  = addr,
        .reg_size   = count,
    };
    esp_err_t err = mbc_master_send_request(&req, data);

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

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t svc_modbus_read_regs(uint8_t slave, svc_modbus_fn_t fn,
                               uint16_t addr, uint16_t count, uint16_t *out)
{
    ESP_RETURN_ON_FALSE(out && count && count <= SVC_MODBUS_MAX_REGS,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(fn == MB_FN_READ_HOLDING || fn == MB_FN_READ_INPUT,
                        ESP_ERR_INVALID_ARG, TAG, "not a register read");

    return transact(slave, (uint8_t)fn, addr, count, out);
}

esp_err_t svc_modbus_read_bits(uint8_t slave, svc_modbus_fn_t fn,
                               uint16_t addr, uint16_t count, uint8_t *out)
{
    ESP_RETURN_ON_FALSE(out && count, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(fn == MB_FN_READ_COILS || fn == MB_FN_READ_DISCRETE,
                        ESP_ERR_INVALID_ARG, TAG, "not a bit read");

    return transact(slave, (uint8_t)fn, addr, count, out);
}

esp_err_t svc_modbus_write_reg(uint8_t slave, uint16_t addr, uint16_t value)
{
    uint16_t v = value;
    return transact(slave, MB_FN_WRITE_SINGLE_REG, addr, 1, &v);
}

esp_err_t svc_modbus_write_regs(uint8_t slave, uint16_t addr,
                                uint16_t count, const uint16_t *values)
{
    ESP_RETURN_ON_FALSE(values && count && count <= SVC_MODBUS_MAX_REGS,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");

    uint16_t buf[SVC_MODBUS_MAX_REGS];
    memcpy(buf, values, count * sizeof(uint16_t));
    return transact(slave, MB_FN_WRITE_MULTI_REGS, addr, count, buf);
}

esp_err_t svc_modbus_write_coil(uint8_t slave, uint16_t addr, bool on)
{
    uint8_t v = on ? 1 : 0;
    return transact(slave, MB_FN_WRITE_SINGLE_COIL, addr, 1, &v);
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
