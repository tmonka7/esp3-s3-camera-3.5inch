/* ST7796 panel driver.
 *
 * In-tree rather than the `espressif/esp_lcd_st7796` managed component, so
 * the project builds with no network access. It implements the standard
 * esp_lcd_panel_t vtable, so the rest of the code uses the ordinary
 * esp_lcd_panel_* calls and nothing else knows the difference.
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates an ST7796 panel on an already-created panel IO handle.
 *
 * @param io          SPI panel IO
 * @param panel_dev   reset pin, colour order and bits per pixel
 * @param ret_panel   receives the new panel handle
 */
esp_err_t lcd_new_panel_st7796(esp_lcd_panel_io_handle_t io,
                               const esp_lcd_panel_dev_config_t *panel_dev,
                               esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
