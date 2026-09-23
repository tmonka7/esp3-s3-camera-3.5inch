#include <string.h>

#include "esp_log.h"

#include "mb_proto.h"
#include "svc_modbus.h"

static const char *TAG = "mb_proto";

uint16_t mb_crc16(const uint8_t *data, size_t len)
{
    /* Bitwise CRC-16/MODBUS. A table would be faster, but at 9600 baud the
     * UART is four orders of magnitude slower than this loop. */
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool mb_is_read_fn(uint8_t fn)
{
    return fn == MB_FN_READ_COILS || fn == MB_FN_READ_DISCRETE ||
           fn == MB_FN_READ_HOLDING || fn == MB_FN_READ_INPUT;
}

size_t mb_build_request(uint8_t *pdu, uint8_t fn, uint16_t addr,
                        uint16_t count, const uint16_t *wr_data)
{
    if (!pdu) {
        return 0;
    }

    pdu[0] = fn;
    pdu[1] = (uint8_t)(addr >> 8);
    pdu[2] = (uint8_t)(addr & 0xFF);

    switch (fn) {
    case MB_FN_READ_COILS:
    case MB_FN_READ_DISCRETE:
    case MB_FN_READ_HOLDING:
    case MB_FN_READ_INPUT:
        pdu[3] = (uint8_t)(count >> 8);
        pdu[4] = (uint8_t)(count & 0xFF);
        return 5;

    case MB_FN_WRITE_SINGLE_COIL:
        if (!wr_data) {
            return 0;
        }
        /* A coil is written as 0xFF00 for on, 0x0000 for off -- not 1/0. */
        pdu[3] = wr_data[0] ? 0xFF : 0x00;
        pdu[4] = 0x00;
        return 5;

    case MB_FN_WRITE_SINGLE_REG:
        if (!wr_data) {
            return 0;
        }
        pdu[3] = (uint8_t)(wr_data[0] >> 8);
        pdu[4] = (uint8_t)(wr_data[0] & 0xFF);
        return 5;

    case MB_FN_WRITE_MULTI_REGS: {
        if (!wr_data || count == 0 || count > 123) {
            return 0;
        }
        pdu[3] = (uint8_t)(count >> 8);
        pdu[4] = (uint8_t)(count & 0xFF);
        pdu[5] = (uint8_t)(count * 2);
        for (uint16_t i = 0; i < count; i++) {
            pdu[6 + i * 2]     = (uint8_t)(wr_data[i] >> 8);
            pdu[6 + i * 2 + 1] = (uint8_t)(wr_data[i] & 0xFF);
        }
        return (size_t)(6 + count * 2);
    }

    case MB_FN_WRITE_MULTI_COILS: {
        if (!wr_data || count == 0 || count > 1968) {
            return 0;
        }
        const uint8_t bytes = (uint8_t)((count + 7) / 8);
        pdu[3] = (uint8_t)(count >> 8);
        pdu[4] = (uint8_t)(count & 0xFF);
        pdu[5] = bytes;
        memset(&pdu[6], 0, bytes);
        for (uint16_t i = 0; i < count; i++) {
            if (wr_data[i]) {
                pdu[6 + i / 8] |= (uint8_t)(1u << (i % 8));
            }
        }
        return (size_t)(6 + bytes);
    }

    default:
        return 0;
    }
}

size_t mb_expected_response_len(uint8_t fn, uint16_t count)
{
    switch (fn) {
    case MB_FN_READ_HOLDING:
    case MB_FN_READ_INPUT:
        return (size_t)(2 + count * 2);              /* fn + bytecount + data */

    case MB_FN_READ_COILS:
    case MB_FN_READ_DISCRETE:
        return (size_t)(2 + (count + 7) / 8);

    case MB_FN_WRITE_SINGLE_COIL:
    case MB_FN_WRITE_SINGLE_REG:
    case MB_FN_WRITE_MULTI_COILS:
    case MB_FN_WRITE_MULTI_REGS:
        return 5;                                    /* fn + addr + value/qty */

    default:
        return 0;
    }
}

esp_err_t mb_parse_response(const uint8_t *pdu, size_t len, uint8_t fn,
                            uint16_t count, void *out)
{
    if (!pdu || len < 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (pdu[0] & MB_EXCEPTION_FLAG) {
        /* pdu[1] is the Modbus exception code: 1 illegal function,
         * 2 illegal address, 3 illegal value, 4 device failure, 6 busy. */
        ESP_LOGW(TAG, "slave returned exception %u for function %u", pdu[1], fn);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (pdu[0] != fn) {
        ESP_LOGW(TAG, "function mismatch: asked %u, got %u", fn, pdu[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!mb_is_read_fn(fn)) {
        return ESP_OK;      /* write echo; nothing to copy out */
    }

    const uint8_t byte_count = pdu[1];
    if (len < (size_t)(2 + byte_count)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (fn == MB_FN_READ_HOLDING || fn == MB_FN_READ_INPUT) {
        if (byte_count != count * 2) {
            ESP_LOGW(TAG, "expected %u data bytes, got %u", count * 2, byte_count);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (out) {
            uint16_t *regs = out;
            for (uint16_t i = 0; i < count; i++) {
                /* Modbus registers are big-endian on the wire. */
                regs[i] = (uint16_t)((pdu[2 + i * 2] << 8) | pdu[2 + i * 2 + 1]);
            }
        }
    } else {
        const uint8_t expect = (uint8_t)((count + 7) / 8);
        if (byte_count != expect) {
            ESP_LOGW(TAG, "expected %u bit bytes, got %u", expect, byte_count);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (out) {
            memcpy(out, &pdu[2], byte_count);
        }
    }

    return ESP_OK;
}
