#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_check.h"

#include "bsp_board.h"
#include "bsp_i2c.h"
#include "bsp_io_expander.h"

static const char *TAG = "bsp_ioexp";

/* TCA9554 register map */
#define TCA9554_REG_INPUT    0x00
#define TCA9554_REG_OUTPUT   0x01
#define TCA9554_REG_POLARITY 0x02
#define TCA9554_REG_CONFIG   0x03   /* 1 = input, 0 = output */

static bool    s_present;
static uint8_t s_output_shadow = 0x00;

/* Bits the board actually drives; everything else stays an input so an
 * unexpected wiring never gets back-driven. */
static uint8_t used_bits_mask(void)
{
    uint8_t mask = 0;
    const int bits[] = {
        CONFIG_BSP_IO_EXP_BIT_LCD_RST,
        CONFIG_BSP_IO_EXP_BIT_LCD_BL,
        CONFIG_BSP_IO_EXP_BIT_TP_RST,
        CONFIG_BSP_IO_EXP_BIT_CAM_PWDN,
    };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if (bits[i] >= 0 && bits[i] < 8) {
            mask |= (uint8_t)(1u << bits[i]);
        }
    }
    return mask;
}

esp_err_t bsp_io_expander_init(void)
{
#if !BSP_USE_IO_EXPANDER
    ESP_LOGI(TAG, "IO expander disabled in Kconfig");
    return ESP_OK;
#else
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");

    if (!bsp_i2c_probe(BSP_I2C_ADDR_TCA9554)) {
        ESP_LOGW(TAG, "no TCA9554 at 0x%02X -- expander-driven lines will be ignored",
                 BSP_I2C_ADDR_TCA9554);
        s_present = false;
        return ESP_OK;
    }

    const uint8_t outputs = used_bits_mask();

    /* Park the outputs low before flipping direction so no line glitches high. */
    s_output_shadow = 0x00;
    ESP_RETURN_ON_ERROR(bsp_i2c_write_reg(BSP_I2C_ADDR_TCA9554, TCA9554_REG_OUTPUT,
                                          s_output_shadow), TAG, "output");
    ESP_RETURN_ON_ERROR(bsp_i2c_write_reg(BSP_I2C_ADDR_TCA9554, TCA9554_REG_CONFIG,
                                          (uint8_t)~outputs), TAG, "config");

    s_present = true;
    ESP_LOGI(TAG, "TCA9554 ready at 0x%02X, outputs=0x%02X",
             BSP_I2C_ADDR_TCA9554, outputs);
    return ESP_OK;
#endif
}

bool bsp_io_expander_present(void)
{
    return s_present;
}

esp_err_t bsp_io_expander_set(int bit, bool level)
{
    if (bit < 0 || bit > 7) {
        return ESP_OK;          /* line not routed through the expander */
    }
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t mask = (uint8_t)(1u << bit);
    const uint8_t next = level ? (uint8_t)(s_output_shadow | mask)
                               : (uint8_t)(s_output_shadow & ~mask);
    if (next == s_output_shadow) {
        return ESP_OK;
    }

    esp_err_t err = bsp_i2c_write_reg(BSP_I2C_ADDR_TCA9554, TCA9554_REG_OUTPUT, next);
    if (err == ESP_OK) {
        s_output_shadow = next;
    }
    return err;
}

esp_err_t bsp_io_expander_get(int bit, bool *level)
{
    if (bit < 0 || bit > 7 || !level) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t in = 0;
    esp_err_t err = bsp_i2c_read_reg(BSP_I2C_ADDR_TCA9554, TCA9554_REG_INPUT, &in);
    if (err == ESP_OK) {
        *level = (in & (1u << bit)) != 0;
    }
    return err;
}
