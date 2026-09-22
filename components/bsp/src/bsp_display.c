#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7796.h"

#include "esp_lcd_touch_ft5x06.h"

#include "esp_lvgl_port.h"

#include "bsp_board.h"
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_io_expander.h"

static const char *TAG = "bsp_disp";

#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES      LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ  20000          /* above the audible range */

static esp_lcd_panel_io_handle_t  s_io_handle;
static esp_lcd_panel_handle_t     s_panel_handle;
static esp_lcd_touch_handle_t     s_touch_handle;
static lv_disp_t                 *s_disp;
static int                        s_brightness = 80;
static bool                       s_bl_is_pwm;

/* --------------------------------------------------------------------------
 * Backlight
 *
 * The schematic may route the backlight either to a GPIO (dimmable with LEDC)
 * or to a TCA9554 bit (on/off only). Both are supported; Kconfig decides.
 * ------------------------------------------------------------------------ */
static esp_err_t backlight_init(void)
{
    if (BSP_LCD_PIN_BL >= 0) {
        const ledc_timer_config_t timer = {
            .speed_mode      = BL_LEDC_MODE,
            .timer_num       = BL_LEDC_TIMER,
            .duty_resolution = BL_LEDC_RES,
            .freq_hz         = BL_LEDC_FREQ_HZ,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

        const ledc_channel_config_t ch = {
            .gpio_num   = BSP_LCD_PIN_BL,
            .speed_mode = BL_LEDC_MODE,
            .channel    = BL_LEDC_CHANNEL,
            .timer_sel  = BL_LEDC_TIMER,
            .duty       = 0,
            .hpoint     = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc channel");
        s_bl_is_pwm = true;
    } else {
        s_bl_is_pwm = false;
    }
    return ESP_OK;
}

esp_err_t bsp_display_set_brightness(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_brightness = percent;

    if (s_bl_is_pwm) {
        const uint32_t max_duty = (1u << BL_LEDC_RES) - 1;
        const uint32_t duty     = (max_duty * (uint32_t)percent) / 100u;
        ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty),
                            TAG, "set duty");
        return ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    }

    /* Expander-driven: on/off is all the hardware offers. */
    return bsp_io_expander_set(CONFIG_BSP_IO_EXP_BIT_LCD_BL, percent > 0);
}

int bsp_display_get_brightness(void)
{
    return s_brightness;
}

/* --------------------------------------------------------------------------
 * Panel
 * ------------------------------------------------------------------------ */
static esp_err_t panel_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num     = BSP_LCD_PIN_SCLK,
        .mosi_io_num     = BSP_LCD_PIN_MOSI,
        .miso_io_num     = BSP_LCD_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        /* One flush carries a full-width band of the framebuffer. */
        .max_transfer_sz = BSP_LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = BSP_LCD_PIN_CS,
        .dc_gpio_num       = BSP_LCD_PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = BSP_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = BSP_LCD_CMD_BITS,
        .lcd_param_bits    = BSP_LCD_PARAM_BITS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST,
                                                 &io_cfg, &s_io_handle),
                        TAG, "panel io");

    /* When RST hangs off the expander, pulse it by hand first: the ST7796
     * driver can only toggle a real GPIO. */
    if (BSP_LCD_PIN_RST < 0 && bsp_io_expander_present()) {
        bsp_io_expander_set(CONFIG_BSP_IO_EXP_BIT_LCD_RST, false);
        vTaskDelay(pdMS_TO_TICKS(20));
        bsp_io_expander_set(CONFIG_BSP_IO_EXP_BIT_LCD_RST, true);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_PIN_RST,
#if CONFIG_BSP_LCD_BGR_ELEMENT_ORDER
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(s_io_handle, &panel_cfg, &s_panel_handle),
                        TAG, "st7796");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, CONFIG_BSP_LCD_SWAP_XY),
                        TAG, "swap_xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle,
                                             CONFIG_BSP_LCD_MIRROR_X,
                                             CONFIG_BSP_LCD_MIRROR_Y),
                        TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel_handle, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "disp on");
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Touch
 * ------------------------------------------------------------------------ */
static esp_err_t touch_init(void)
{
    if (BSP_TOUCH_PIN_RST < 0 && bsp_io_expander_present()) {
        bsp_io_expander_set(CONFIG_BSP_IO_EXP_BIT_TP_RST, false);
        vTaskDelay(pdMS_TO_TICKS(10));
        bsp_io_expander_set(CONFIG_BSP_IO_EXP_BIT_TP_RST, true);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((uint32_t)BSP_I2C_PORT,
                                                 &tp_io_cfg, &tp_io),
                        TAG, "touch io");

    /* The controller reports raw panel coordinates, so it gets the panel
     * geometry plus the same transform the display uses. */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BSP_LCD_H_RES,
        .y_max        = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_TOUCH_PIN_RST,
        .int_gpio_num = BSP_TOUCH_PIN_INT,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = CONFIG_BSP_LCD_SWAP_XY,
            .mirror_x = CONFIG_BSP_LCD_MIRROR_X,
            .mirror_y = CONFIG_BSP_LCD_MIRROR_Y,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch_handle),
                        TAG, "ft5x06");
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * LVGL
 * ------------------------------------------------------------------------ */
static esp_err_t lvgl_init(void)
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority     = 4,
        .task_stack        = 8192,
        .task_affinity     = 0,     /* core 0; the camera pipeline owns core 1 */
        .task_max_sleep_ms = 500,
        .timer_period_ms   = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = s_io_handle,
        .panel_handle  = s_panel_handle,
        /* A 1/6-screen band of internal DMA-capable RAM: small enough to
         * allocate reliably, large enough that a flush is a single transfer. */
        .buffer_size   = BSP_LCD_H_RES * 80,
        .double_buffer = true,
        .hres          = BSP_UI_H_RES,
        .vres          = BSP_UI_V_RES,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = CONFIG_BSP_LCD_SWAP_XY,
            .mirror_x = CONFIG_BSP_LCD_MIRROR_X,
            .mirror_y = CONFIG_BSP_LCD_MIRROR_Y,
        },
        .flags = {
            .buff_dma    = true,
            .buff_spiram = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "lvgl_port_add_disp");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = s_disp,
        .handle = s_touch_handle,
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg), ESP_FAIL, TAG,
                        "lvgl_port_add_touch");
    return ESP_OK;
}

esp_err_t bsp_display_init(void)
{
    if (s_disp) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_board_init(), TAG, "board");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    /* Stay dark until LVGL has drawn the first frame, otherwise the user sees
     * a screenful of uninitialised panel RAM. */
    bsp_display_set_brightness(0);

    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");
    ESP_RETURN_ON_ERROR(lvgl_init(), TAG, "lvgl");

    ESP_LOGI(TAG, "display ready: %dx%d", BSP_UI_H_RES, BSP_UI_V_RES);
    return ESP_OK;
}

lv_disp_t *bsp_display_lv_disp(void)
{
    return s_disp;
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}
