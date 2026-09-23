#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"

#include "bsp_board.h"
#include "bsp_i2c.h"
#include "touch_ft6336.h"

static const char *TAG = "ft6336";

/* ---- register map ------------------------------------------------------ */
#define REG_DEV_MODE     0x00
#define REG_TD_STATUS    0x02   /* low nibble = number of points held  */
#define REG_P1_XH        0x03   /* 6 bytes per point from here         */
#define REG_TH_GROUP     0x80   /* touch detect threshold              */
#define REG_PERIOD_ACTIVE 0x88  /* report rate while touched           */
#define REG_CHIP_ID      0xA3
#define REG_FIRMWARE_ID  0xA6
#define REG_VENDOR_ID    0xA8

static touch_ft6336_config_t s_cfg;
static bool                  s_ready;

esp_err_t touch_ft6336_init(const touch_ft6336_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "null config");

    s_cfg = *cfg;

    if (s_cfg.rst_gpio >= 0) {
        const gpio_config_t rst = {
            .mode         = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << s_cfg.rst_gpio,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "reset gpio");

        gpio_set_level(s_cfg.rst_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(s_cfg.rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(120));   /* the controller needs ~100 ms */
    }

    if (s_cfg.int_gpio >= 0) {
        /* Input only. The INT line would let us skip polls, but LVGL already
         * polls the indev on a timer, so reading the status register is
         * simpler and costs one I2C transfer every 20 ms. */
        const gpio_config_t irq = {
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pin_bit_mask = 1ULL << s_cfg.int_gpio,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&irq), TAG, "int gpio");
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_FALSE(bsp_i2c_probe(BSP_I2C_ADDR_FT6336), ESP_ERR_NOT_FOUND,
                        TAG, "no touch controller at 0x%02X", BSP_I2C_ADDR_FT6336);

    uint8_t chip = 0, vendor = 0, fw = 0;
    bsp_i2c_read_reg(BSP_I2C_ADDR_FT6336, REG_CHIP_ID,     &chip);
    bsp_i2c_read_reg(BSP_I2C_ADDR_FT6336, REG_VENDOR_ID,   &vendor);
    bsp_i2c_read_reg(BSP_I2C_ADDR_FT6336, REG_FIRMWARE_ID, &fw);

    /* Normal operating mode, not factory/test. */
    bsp_i2c_write_reg(BSP_I2C_ADDR_FT6336, REG_DEV_MODE, 0x00);
    /* A lower threshold makes light taps register; 22 is the part default. */
    bsp_i2c_write_reg(BSP_I2C_ADDR_FT6336, REG_TH_GROUP, 22);
    /* ~60 Hz while a finger is down, which comfortably outpaces LVGL. */
    bsp_i2c_write_reg(BSP_I2C_ADDR_FT6336, REG_PERIOD_ACTIVE, 12);

    s_ready = true;
    ESP_LOGI(TAG, "touch ready (chip 0x%02X, vendor 0x%02X, fw 0x%02X)",
             chip, vendor, fw);
    return ESP_OK;
}

bool touch_ft6336_read(uint16_t *out_x, uint16_t *out_y)
{
    if (!s_ready || !out_x || !out_y) {
        return false;
    }

    /* One transfer covers the status byte and the first point. */
    const uint8_t reg = REG_TD_STATUS;
    uint8_t       buf[7] = {0};
    if (bsp_i2c_write_read(BSP_I2C_ADDR_FT6336, &reg, 1, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }

    const uint8_t points = buf[0] & 0x0F;
    if (points == 0 || points > 2) {
        return false;      /* 0x0F shows up on a bad read; treat it as no touch */
    }

    /* buf[1..4] is P1: XH, XL, YH, YL -- the high bytes carry flags in the
     * top nibble, so only the low 4 bits are coordinate. */
    uint16_t x = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    uint16_t y = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);

    if (x >= s_cfg.x_max) x = s_cfg.x_max ? s_cfg.x_max - 1 : 0;
    if (y >= s_cfg.y_max) y = s_cfg.y_max ? s_cfg.y_max - 1 : 0;

    /* Mirror before swapping, so the flags mean the same thing they do in
     * the display configuration they are copied from. */
    if (s_cfg.mirror_x) x = (uint16_t)(s_cfg.x_max - 1 - x);
    if (s_cfg.mirror_y) y = (uint16_t)(s_cfg.y_max - 1 - y);

    if (s_cfg.swap_xy) {
        const uint16_t t = x;
        x = y;
        y = t;
    }

    *out_x = x;
    *out_y = y;
    return true;
}
