#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "bsp_board.h"
#include "bsp_i2c.h"

static const char *TAG = "bsp_i2c";

#define I2C_TIMEOUT pdMS_TO_TICKS(100)

static bool              s_inited;
static SemaphoreHandle_t s_lock;

esp_err_t bsp_i2c_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    const i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = BSP_I2C_PIN_SDA,
        .scl_io_num       = BSP_I2C_PIN_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BSP_I2C_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(BSP_I2C_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(BSP_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "I2C%d up on SDA=%d SCL=%d @ %d Hz",
             BSP_I2C_PORT, BSP_I2C_PIN_SDA, BSP_I2C_PIN_SCL, BSP_I2C_FREQ_HZ);
    return ESP_OK;
}

i2c_port_t bsp_i2c_port(void)
{
    return (i2c_port_t)BSP_I2C_PORT;
}

/* The camera's SCCB, the touch panel and four housekeeping chips all share one
 * bus, and the touch driver is polled from the LVGL task while services poll
 * the RTC and the PMU. Serialising here keeps those transfers from
 * interleaving. */
static inline bool lock(void)
{
    return s_inited && xSemaphoreTake(s_lock, I2C_TIMEOUT) == pdTRUE;
}

static inline void unlock(void)
{
    xSemaphoreGive(s_lock);
}

esp_err_t bsp_i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    if (!lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_write_to_device(BSP_I2C_PORT, addr, data, len, I2C_TIMEOUT);
    unlock();
    return err;
}

esp_err_t bsp_i2c_read(uint8_t addr, uint8_t *data, size_t len)
{
    if (!lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_read_from_device(BSP_I2C_PORT, addr, data, len, I2C_TIMEOUT);
    unlock();
    return err;
}

esp_err_t bsp_i2c_write_read(uint8_t addr, const uint8_t *tx, size_t tx_len,
                             uint8_t *rx, size_t rx_len)
{
    if (!lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_write_read_device(BSP_I2C_PORT, addr, tx, tx_len,
                                                 rx, rx_len, I2C_TIMEOUT);
    unlock();
    return err;
}

esp_err_t bsp_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return bsp_i2c_write(addr, buf, sizeof(buf));
}

esp_err_t bsp_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *val)
{
    return bsp_i2c_write_read(addr, &reg, 1, val, 1);
}

bool bsp_i2c_probe(uint8_t addr)
{
    if (!lock()) {
        return false;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(BSP_I2C_PORT, cmd, I2C_TIMEOUT);
    i2c_cmd_link_delete(cmd);

    unlock();
    return err == ESP_OK;
}
