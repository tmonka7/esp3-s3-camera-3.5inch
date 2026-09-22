#pragma once

#include <stdint.h>
#include <stddef.h>
#include "driver/i2c.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Installs the shared I2C master. Idempotent. */
esp_err_t bsp_i2c_init(void);

/** The port the shared bus lives on, for drivers that want a handle. */
i2c_port_t bsp_i2c_port(void);

/** Serialised register helpers -- every on-board chip shares one bus. */
esp_err_t bsp_i2c_write(uint8_t addr, const uint8_t *data, size_t len);
esp_err_t bsp_i2c_read(uint8_t addr, uint8_t *data, size_t len);
esp_err_t bsp_i2c_write_read(uint8_t addr, const uint8_t *tx, size_t tx_len,
                             uint8_t *rx, size_t rx_len);
esp_err_t bsp_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val);
esp_err_t bsp_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *val);

/** True when a device ACKs its address. Used for optional-chip probing. */
bool bsp_i2c_probe(uint8_t addr);

#ifdef __cplusplus
}
#endif
