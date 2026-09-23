/* FT6336 capacitive touch controller.
 *
 * In-tree rather than `espressif/esp_lcd_touch_ft5x06` + `esp_lcd_touch`, so
 * the project builds offline. The panel only ever needs one finger, so this
 * reports a single point and skips the multi-touch bookkeeping the generic
 * driver carries.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t x_max;        /* panel width in its native orientation  */
    uint16_t y_max;        /* panel height in its native orientation */
    int      rst_gpio;     /* -1 when reset is on the IO expander    */
    int      int_gpio;     /* -1 to poll                             */
    bool     swap_xy;
    bool     mirror_x;
    bool     mirror_y;
} touch_ft6336_config_t;

/** Probes the controller and applies `cfg`. */
esp_err_t touch_ft6336_init(const touch_ft6336_config_t *cfg);

/**
 * Reads the current touch point, already transformed to screen coordinates.
 *
 * @return true while a finger is down, false otherwise.
 */
bool touch_ft6336_read(uint16_t *out_x, uint16_t *out_y);

#ifdef __cplusplus
}
#endif
