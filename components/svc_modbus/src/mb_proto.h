/* Modbus PDU encoding, shared by the RTU and TCP transports.
 *
 * Only the master side is implemented, and only the function codes the UI
 * exposes. Everything here is transport-agnostic: an RTU frame is this PDU
 * with an address byte in front and a CRC behind it, a TCP frame is this PDU
 * behind an MBAP header.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A read of 125 registers is the protocol maximum; the PDU is then
 * 1 + 1 + 250 = 252 bytes, plus the RTU address and CRC. */
#define MB_PDU_MAX      253
#define MB_RTU_ADU_MAX  256
#define MB_TCP_ADU_MAX  260

#define MB_EXCEPTION_FLAG   0x80

uint16_t mb_crc16(const uint8_t *data, size_t len);

/**
 * Builds a request PDU.
 *
 * @param wr_data  for fn 5/6 a single uint16_t, for fn 15/16 an array of
 *                 `count` uint16_t values; ignored by the read functions.
 * @return PDU length, or 0 when the function code is not supported.
 */
size_t mb_build_request(uint8_t *pdu, uint8_t fn, uint16_t addr,
                        uint16_t count, const uint16_t *wr_data);

/** Total response PDU length expected for a successful `fn`, 0 if unknown. */
size_t mb_expected_response_len(uint8_t fn, uint16_t count);

/**
 * Validates a response PDU and copies its payload out.
 *
 * @param out  uint16_t[count] for register reads, a bitmap for bit reads,
 *             NULL for writes.
 */
esp_err_t mb_parse_response(const uint8_t *pdu, size_t len, uint8_t fn,
                            uint16_t count, void *out);

/** True when the function code returns register/bit data rather than an echo. */
bool mb_is_read_fn(uint8_t fn);

#ifdef __cplusplus
}
#endif
