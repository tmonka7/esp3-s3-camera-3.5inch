/* Board definition for the Waveshare ESP32-S3-Touch-LCD-3.5-C.
 *
 * Every pin lives in Kconfig ("Board Support" menu) so a schematic revision
 * never forces a code change. This header is the only place that turns the
 * CONFIG_* symbols into the names the rest of the firmware uses.
 */
#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Kconfig booleans -------------------------------------------------- *
 *
 * A `bool` Kconfig option is #defined as 1 when set and is NOT DEFINED AT ALL
 * when cleared. That is fine inside #if, where an unknown identifier is 0,
 * but it is a compile error anywhere it appears in ordinary C -- which is how
 * these are used (struct initialisers, function arguments). Normalising them
 * once here means the rest of the firmware can treat them as plain 0/1, and
 * that unticking one in menuconfig can never break the build.
 */
#ifdef CONFIG_BSP_PINS_VERIFIED
#define BSP_PINS_VERIFIED            1
#else
#define BSP_PINS_VERIFIED            0
#endif

#ifdef CONFIG_BSP_LCD_SWAP_XY
#define BSP_LCD_SWAP_XY              1
#else
#define BSP_LCD_SWAP_XY              0
#endif

#ifdef CONFIG_BSP_LCD_MIRROR_X
#define BSP_LCD_MIRROR_X             1
#else
#define BSP_LCD_MIRROR_X             0
#endif

#ifdef CONFIG_BSP_LCD_MIRROR_Y
#define BSP_LCD_MIRROR_Y             1
#else
#define BSP_LCD_MIRROR_Y             0
#endif

#ifdef CONFIG_BSP_LCD_BGR_ELEMENT_ORDER
#define BSP_LCD_BGR_ELEMENT_ORDER    1
#else
#define BSP_LCD_BGR_ELEMENT_ORDER    0
#endif

#ifdef CONFIG_BSP_USE_IO_EXPANDER
#define BSP_USE_IO_EXPANDER          1
#else
#define BSP_USE_IO_EXPANDER          0
#endif

#ifdef CONFIG_BSP_SD_FORMAT_IF_MOUNT_FAILED
#define BSP_SD_FORMAT_IF_MOUNT_FAILED 1
#else
#define BSP_SD_FORMAT_IF_MOUNT_FAILED 0
#endif

/* ---- display ---------------------------------------------------------- */
#define BSP_LCD_H_RES              CONFIG_BSP_LCD_H_RES
#define BSP_LCD_V_RES              CONFIG_BSP_LCD_V_RES
#define BSP_LCD_SPI_HOST           CONFIG_BSP_LCD_SPI_HOST
#define BSP_LCD_PIN_SCLK           CONFIG_BSP_LCD_PIN_SCLK
#define BSP_LCD_PIN_MOSI           CONFIG_BSP_LCD_PIN_MOSI
#define BSP_LCD_PIN_MISO           CONFIG_BSP_LCD_PIN_MISO
#define BSP_LCD_PIN_CS             CONFIG_BSP_LCD_PIN_CS
#define BSP_LCD_PIN_DC             CONFIG_BSP_LCD_PIN_DC
#define BSP_LCD_PIN_RST            CONFIG_BSP_LCD_PIN_RST
#define BSP_LCD_PIN_BL             CONFIG_BSP_LCD_PIN_BL
#define BSP_LCD_PIXEL_CLOCK_HZ     CONFIG_BSP_LCD_PIXEL_CLOCK_HZ
#define BSP_LCD_CMD_BITS           8
#define BSP_LCD_PARAM_BITS         8

/* The panel is a 320x480 portrait glass; the UI is designed landscape, so the
 * effective canvas is 480x320 once swap_xy is applied. */
#if BSP_LCD_SWAP_XY
#define BSP_UI_H_RES               BSP_LCD_V_RES
#define BSP_UI_V_RES               BSP_LCD_H_RES
#else
#define BSP_UI_H_RES               BSP_LCD_H_RES
#define BSP_UI_V_RES               BSP_LCD_V_RES
#endif

/* ---- I2C -------------------------------------------------------------- */
#define BSP_I2C_PORT               CONFIG_BSP_I2C_PORT
#define BSP_I2C_PIN_SDA            CONFIG_BSP_I2C_PIN_SDA
#define BSP_I2C_PIN_SCL            CONFIG_BSP_I2C_PIN_SCL
#define BSP_I2C_FREQ_HZ            CONFIG_BSP_I2C_FREQ_HZ

/* Known addresses on the shared bus. */
#define BSP_I2C_ADDR_FT6336        0x38
#define BSP_I2C_ADDR_TCA9554       CONFIG_BSP_IO_EXPANDER_ADDR
#define BSP_I2C_ADDR_PCF85063      0x51
#define BSP_I2C_ADDR_AXP2101       0x34
#define BSP_I2C_ADDR_QMI8658       0x6B
#define BSP_I2C_ADDR_SHT3X         0x44   /* optional external T/H sensor */

/* ---- touch ------------------------------------------------------------ */
#define BSP_TOUCH_PIN_INT          CONFIG_BSP_TOUCH_PIN_INT
#define BSP_TOUCH_PIN_RST          CONFIG_BSP_TOUCH_PIN_RST

/* ---- camera ----------------------------------------------------------- */
#define BSP_CAM_PIN_XCLK           CONFIG_BSP_CAM_PIN_XCLK
#define BSP_CAM_PIN_PCLK           CONFIG_BSP_CAM_PIN_PCLK
#define BSP_CAM_PIN_VSYNC          CONFIG_BSP_CAM_PIN_VSYNC
#define BSP_CAM_PIN_HREF           CONFIG_BSP_CAM_PIN_HREF
#define BSP_CAM_PIN_D0             CONFIG_BSP_CAM_PIN_D0
#define BSP_CAM_PIN_D1             CONFIG_BSP_CAM_PIN_D1
#define BSP_CAM_PIN_D2             CONFIG_BSP_CAM_PIN_D2
#define BSP_CAM_PIN_D3             CONFIG_BSP_CAM_PIN_D3
#define BSP_CAM_PIN_D4             CONFIG_BSP_CAM_PIN_D4
#define BSP_CAM_PIN_D5             CONFIG_BSP_CAM_PIN_D5
#define BSP_CAM_PIN_D6             CONFIG_BSP_CAM_PIN_D6
#define BSP_CAM_PIN_D7             CONFIG_BSP_CAM_PIN_D7
#define BSP_CAM_PIN_SIOD           CONFIG_BSP_CAM_PIN_SIOD
#define BSP_CAM_PIN_SIOC           CONFIG_BSP_CAM_PIN_SIOC
#define BSP_CAM_PIN_PWDN           CONFIG_BSP_CAM_PIN_PWDN
#define BSP_CAM_PIN_RESET          CONFIG_BSP_CAM_PIN_RESET
#define BSP_CAM_XCLK_FREQ_HZ       CONFIG_BSP_CAM_XCLK_FREQ_HZ

/* ---- TF card ---------------------------------------------------------- */
#define BSP_SD_PIN_CLK             CONFIG_BSP_SD_PIN_CLK
#define BSP_SD_PIN_CMD             CONFIG_BSP_SD_PIN_CMD
#define BSP_SD_PIN_D0              CONFIG_BSP_SD_PIN_D0
#define BSP_SD_MOUNT_POINT         "/sdcard"

/* ---- serial ----------------------------------------------------------- */
#define BSP_RS485_UART_NUM         CONFIG_BSP_RS485_UART_NUM
#define BSP_RS485_PIN_TX           CONFIG_BSP_RS485_PIN_TX
#define BSP_RS485_PIN_RX           CONFIG_BSP_RS485_PIN_RX
#define BSP_RS485_PIN_DE           CONFIG_BSP_RS485_PIN_DE
#define BSP_UART_NUM               CONFIG_BSP_UART_NUM
#define BSP_UART_PIN_TX            CONFIG_BSP_UART_PIN_TX
#define BSP_UART_PIN_RX            CONFIG_BSP_UART_PIN_RX

#define BSP_BUZZER_PIN             CONFIG_BSP_BUZZER_PIN

/** Brings up the shared I2C bus and the IO expander. Safe to call twice. */
esp_err_t bsp_board_init(void);

#ifdef __cplusplus
}
#endif
