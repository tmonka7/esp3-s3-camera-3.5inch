#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/uart.h"

#include "bsp_board.h"
#include "mb_proto.h"
#include "mb_rtu.h"
#include "svc_modbus.h"

static const char *TAG = "mb_rtu";

#define UART_RX_BUF      512
#define RESPONSE_TOUT_MS 400

static bool s_up;

esp_err_t mb_rtu_start(uint32_t baud, uint8_t parity)
{
    if (s_up) {
        mb_rtu_stop();
    }

    const uart_config_t uc = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = (parity == 1) ? UART_PARITY_ODD
                   : (parity == 2) ? UART_PARITY_EVEN
                                   : UART_PARITY_DISABLE,
        /* Modbus requires two stop bits when there is no parity bit, so the
         * character is always 11 bits on the wire. */
        .stop_bits = (parity == 0) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(BSP_RS485_UART_NUM, UART_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "driver install");
    }

    ESP_RETURN_ON_ERROR(uart_param_config(BSP_RS485_UART_NUM, &uc), TAG, "param");
    ESP_RETURN_ON_ERROR(uart_set_pin(BSP_RS485_UART_NUM,
                                     BSP_RS485_PIN_TX, BSP_RS485_PIN_RX,
                                     BSP_RS485_PIN_DE >= 0 ? BSP_RS485_PIN_DE
                                                           : UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "pins");

    /* In half-duplex mode the UART drives RTS (wired to DE/RE) around each
     * transmission by itself, which is exactly the turnaround RS485 needs. */
    ESP_RETURN_ON_ERROR(uart_set_mode(BSP_RS485_UART_NUM,
                                      BSP_RS485_PIN_DE >= 0 ? UART_MODE_RS485_HALF_DUPLEX
                                                            : UART_MODE_UART),
                        TAG, "mode");

    uart_flush_input(BSP_RS485_UART_NUM);

    s_up = true;
    ESP_LOGI(TAG, "RTU master on UART%d @ %lu, DE %d",
             BSP_RS485_UART_NUM, (unsigned long)baud, BSP_RS485_PIN_DE);
    return ESP_OK;
}

esp_err_t mb_rtu_stop(void)
{
    if (!s_up) {
        return ESP_OK;
    }
    s_up = false;
    return uart_driver_delete(BSP_RS485_UART_NUM);
}

bool mb_rtu_is_up(void)
{
    return s_up;
}

/** Reads exactly `want` bytes, or fails. */
static esp_err_t read_exact(uint8_t *dst, size_t want, TickType_t deadline)
{
    size_t got = 0;

    while (got < want) {
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            return ESP_ERR_TIMEOUT;
        }

        const int n = uart_read_bytes(BSP_RS485_UART_NUM, dst + got, want - got,
                                      deadline - now);
        if (n <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        got += (size_t)n;
    }
    return ESP_OK;
}

esp_err_t mb_rtu_transact(uint8_t slave, uint8_t fn, uint16_t addr,
                          uint16_t count, const uint16_t *wr_data, void *out)
{
    ESP_RETURN_ON_FALSE(s_up, ESP_ERR_INVALID_STATE, TAG, "RTU is down");

    /* ---- build the ADU ---- */
    uint8_t adu[MB_RTU_ADU_MAX];
    adu[0] = slave;

    const size_t pdu_len = mb_build_request(&adu[1], fn, addr, count, wr_data);
    ESP_RETURN_ON_FALSE(pdu_len, ESP_ERR_NOT_SUPPORTED, TAG, "function %u", fn);

    const uint16_t crc = mb_crc16(adu, 1 + pdu_len);
    adu[1 + pdu_len]     = (uint8_t)(crc & 0xFF);     /* CRC is little-endian */
    adu[1 + pdu_len + 1] = (uint8_t)(crc >> 8);

    const size_t adu_len = 1 + pdu_len + 2;

    /* Anything still in the buffer is a leftover from a timed-out exchange
     * and would be misread as the head of this response. */
    uart_flush_input(BSP_RS485_UART_NUM);

    const int written = uart_write_bytes(BSP_RS485_UART_NUM, adu, adu_len);
    ESP_RETURN_ON_FALSE(written == (int)adu_len, ESP_FAIL, TAG, "write failed");

    /* A broadcast is never answered. */
    if (slave == 0) {
        return ESP_OK;
    }

    /* ---- read the response ---- */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RESPONSE_TOUT_MS);

    uint8_t resp[MB_RTU_ADU_MAX];
    ESP_RETURN_ON_ERROR(read_exact(resp, 2, deadline), TAG, "no response");

    /* An exception reply is shorter than a normal one, so the length can only
     * be decided after the function byte is in hand. */
    size_t body;
    if (resp[1] & MB_EXCEPTION_FLAG) {
        body = 1;
    } else {
        const size_t expect = mb_expected_response_len(fn, count);
        ESP_RETURN_ON_FALSE(expect >= 1, ESP_ERR_NOT_SUPPORTED, TAG, "bad length");
        body = expect - 1;            /* the function byte is already read */
    }

    const size_t remaining = body + 2;   /* + CRC */
    ESP_RETURN_ON_FALSE(2 + remaining <= sizeof(resp), ESP_ERR_INVALID_SIZE,
                        TAG, "response too long");
    ESP_RETURN_ON_ERROR(read_exact(&resp[2], remaining, deadline), TAG, "short response");

    const size_t total = 2 + remaining;

    /* ---- validate ---- */
    const uint16_t got_crc = (uint16_t)(resp[total - 2] | (resp[total - 1] << 8));
    const uint16_t want_crc = mb_crc16(resp, total - 2);
    if (got_crc != want_crc) {
        ESP_LOGW(TAG, "CRC mismatch (got 0x%04X, computed 0x%04X)", got_crc, want_crc);
        return ESP_ERR_INVALID_CRC;
    }

    if (resp[0] != slave) {
        ESP_LOGW(TAG, "reply came from slave %u, expected %u", resp[0], slave);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return mb_parse_response(&resp[1], total - 3, fn, count, out);
}
