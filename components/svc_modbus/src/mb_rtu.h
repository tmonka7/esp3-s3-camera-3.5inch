#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Installs the RS485 UART in half-duplex mode. */
esp_err_t mb_rtu_start(uint32_t baud, uint8_t parity);
esp_err_t mb_rtu_stop(void);
bool      mb_rtu_is_up(void);

/**
 * One request/response exchange. Blocking, and serialised by the caller.
 *
 * @param out  uint16_t[count] for register reads, a bitmap for bit reads,
 *             NULL for writes.
 */
esp_err_t mb_rtu_transact(uint8_t slave, uint8_t fn, uint16_t addr,
                          uint16_t count, const uint16_t *wr_data, void *out);

#ifdef __cplusplus
}
#endif
