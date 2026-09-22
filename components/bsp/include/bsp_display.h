#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Brings up SPI, the ST7796 panel, the FT6336 touch controller and
 * esp_lvgl_port, and returns with LVGL running on its own task.
 */
esp_err_t bsp_display_init(void);

/** The LVGL display created by bsp_display_init(). */
lv_disp_t *bsp_display_lv_disp(void);

/** Backlight duty in percent (0..100). 0 powers the backlight down. */
esp_err_t bsp_display_set_brightness(int percent);
int       bsp_display_get_brightness(void);

/**
 * Take the LVGL mutex. Every touch of an lv_obj from outside the LVGL task
 * must be wrapped in this pair.
 *
 * @param timeout_ms 0 waits forever.
 */
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);

#ifdef __cplusplus
}
#endif
