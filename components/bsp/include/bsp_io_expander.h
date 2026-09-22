#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configures every used TCA9554 bit as an output driven low. */
esp_err_t bsp_io_expander_init(void);

/** True when a TCA9554 answered on the bus at init time. */
bool bsp_io_expander_present(void);

/** Drives one expander bit. `bit` < 0 is a no-op returning ESP_OK, so callers
 *  can pass an unconfigured Kconfig pin straight through. */
esp_err_t bsp_io_expander_set(int bit, bool level);

esp_err_t bsp_io_expander_get(int bit, bool *level);

#ifdef __cplusplus
}
#endif
