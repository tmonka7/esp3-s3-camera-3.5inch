#include "esp_log.h"
#include "esp_check.h"

#include "bsp_board.h"
#include "bsp_i2c.h"
#include "bsp_io_expander.h"

static const char *TAG = "bsp_board";

static bool s_inited;

/* Chips the board is documented to carry. Probing them costs a few
 * milliseconds and turns "the touch panel is dead" into a one-line answer. */
static void probe_known_devices(void)
{
    const struct {
        uint8_t     addr;
        const char *name;
    } devices[] = {
        { BSP_I2C_ADDR_FT6336,   "FT6336 touch"   },
        { BSP_I2C_ADDR_TCA9554,  "TCA9554 expander" },
        { BSP_I2C_ADDR_PCF85063, "PCF85063 RTC"   },
        { BSP_I2C_ADDR_AXP2101,  "AXP2101 PMU"    },
        { BSP_I2C_ADDR_QMI8658,  "QMI8658 IMU"    },
        { BSP_I2C_ADDR_SHT3X,    "SHT3x T/H (optional)" },
    };

    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        ESP_LOGI(TAG, "  0x%02X %-22s %s", devices[i].addr, devices[i].name,
                 bsp_i2c_probe(devices[i].addr) ? "present" : "-");
    }
}

esp_err_t bsp_board_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

#if !CONFIG_BSP_PINS_VERIFIED
    ESP_LOGW(TAG, "================================================================");
    ESP_LOGW(TAG, " Board pins have NOT been confirmed against your schematic.");
    ESP_LOGW(TAG, " The camera DVP pins come from the Waveshare wiki and are good;");
    ESP_LOGW(TAG, " the LCD / I2C / TF / RS485 pins are defaults only. Check them");
    ESP_LOGW(TAG, " in `idf.py menuconfig` -> Board Support, then enable");
    ESP_LOGW(TAG, " BSP_PINS_VERIFIED to silence this.");
    ESP_LOGW(TAG, "================================================================");
#endif

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    probe_known_devices();
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "io expander");

    s_inited = true;
    return ESP_OK;
}
