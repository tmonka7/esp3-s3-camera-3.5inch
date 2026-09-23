#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "lcd_st7796.h"

static const char *TAG = "st7796";

/* ---- command set ------------------------------------------------------- */
#define CMD_SWRESET   0x01
#define CMD_SLPIN     0x10
#define CMD_SLPOUT    0x11
#define CMD_INVOFF    0x20
#define CMD_INVON     0x21
#define CMD_DISPOFF   0x28
#define CMD_DISPON    0x29
#define CMD_CASET     0x2A
#define CMD_RASET     0x2B
#define CMD_RAMWR     0x2C
#define CMD_MADCTL    0x36
#define CMD_COLMOD    0x3A
#define CMD_CSCON     0xF0   /* command-set control (unlocks the extended set) */

/* MADCTL bits */
#define MADCTL_MY     0x80
#define MADCTL_MX     0x40
#define MADCTL_MV     0x20
#define MADCTL_BGR    0x08

typedef struct {
    esp_lcd_panel_t           base;
    esp_lcd_panel_io_handle_t io;
    int                       reset_gpio_num;
    bool                      reset_level;
    uint8_t                   madctl;        /* current MADCTL shadow */
    uint8_t                   colmod;
    int                       x_gap;
    int                       y_gap;
    bool                      swap_axes;
} st7796_panel_t;

static inline st7796_panel_t *to_panel(esp_lcd_panel_t *p)
{
    return __containerof(p, st7796_panel_t, base);
}

/* --------------------------------------------------------------------------
 * Init sequence
 *
 * The extended register block (0xB4..0xE8) is only writable between the two
 * CSCON unlock pairs, which is why the gamma and power settings are bracketed
 * by 0xC3/0x96 and 0x3C/0x69.
 * ------------------------------------------------------------------------ */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;          /* bytes in data */
    uint8_t delay_ms;
} init_cmd_t;

static const init_cmd_t k_init[] = {
    { CMD_CSCON,  { 0xC3 }, 1, 0 },                       /* unlock part I   */
    { CMD_CSCON,  { 0x96 }, 1, 0 },                       /* unlock part II  */

    { 0xB4,       { 0x01 }, 1, 0 },                       /* 1-dot inversion */
    { 0xB6,       { 0x80, 0x02, 0x3B }, 3, 0 },           /* display function*/
    { 0xE8,       { 0x40, 0x8A, 0x00, 0x00, 0x29,
                    0x19, 0xA5, 0x33 }, 8, 0 },           /* output adjust   */
    { 0xC1,       { 0x06 }, 1, 0 },                       /* power control 2 */
    { 0xC2,       { 0xA7 }, 1, 0 },                       /* power control 3 */
    { 0xC5,       { 0x18 }, 1, 120 },                     /* VCOM            */

    { 0xE0,       { 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F,
                    0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B }, 14, 0 },
    { 0xE1,       { 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B,
                    0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B }, 14, 120 },

    { CMD_CSCON,  { 0x3C }, 1, 0 },                       /* lock part I     */
    { CMD_CSCON,  { 0x69 }, 1, 120 },                     /* lock part II    */
};

/* --------------------------------------------------------------------------
 * vtable
 * ------------------------------------------------------------------------ */
static esp_err_t panel_reset(esp_lcd_panel_t *panel)
{
    st7796_panel_t *p = to_panel(panel);

    if (p->reset_gpio_num >= 0) {
        gpio_set_level(p->reset_gpio_num, p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(p->reset_gpio_num, !p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, CMD_SWRESET, NULL, 0),
                            TAG, "swreset");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_init(esp_lcd_panel_t *panel)
{
    st7796_panel_t *p = to_panel(panel);

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, CMD_SLPOUT, NULL, 0),
                        TAG, "sleep out");
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, CMD_MADCTL, &p->madctl, 1),
                        TAG, "madctl");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, CMD_COLMOD, &p->colmod, 1),
                        TAG, "colmod");

    for (size_t i = 0; i < sizeof(k_init) / sizeof(k_init[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, k_init[i].cmd,
                                                      k_init[i].data, k_init[i].len),
                            TAG, "init cmd 0x%02X", k_init[i].cmd);
        if (k_init[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(k_init[i].delay_ms));
        }
    }

    ESP_LOGI(TAG, "panel initialised (MADCTL 0x%02X)", p->madctl);
    return ESP_OK;
}

static esp_err_t panel_del(esp_lcd_panel_t *panel)
{
    st7796_panel_t *p = to_panel(panel);

    if (p->reset_gpio_num >= 0) {
        gpio_reset_pin(p->reset_gpio_num);
    }
    free(p);
    return ESP_OK;
}

static esp_err_t panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                   int x_end, int y_end, const void *color_data)
{
    st7796_panel_t *p = to_panel(panel);

    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end, ESP_ERR_INVALID_ARG,
                        TAG, "empty area");

    x_start += p->x_gap;
    x_end   += p->x_gap;
    y_start += p->y_gap;
    y_end   += p->y_gap;

    /* CASET/RASET take inclusive end columns, hence the -1. */
    const uint8_t caset[4] = {
        (uint8_t)(x_start >> 8), (uint8_t)(x_start & 0xFF),
        (uint8_t)((x_end - 1) >> 8), (uint8_t)((x_end - 1) & 0xFF),
    };
    const uint8_t raset[4] = {
        (uint8_t)(y_start >> 8), (uint8_t)(y_start & 0xFF),
        (uint8_t)((y_end - 1) >> 8), (uint8_t)((y_end - 1) & 0xFF),
    };

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, CMD_CASET, caset, 4),
                        TAG, "caset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, CMD_RASET, raset, 4),
                        TAG, "raset");

    const size_t len = (size_t)(x_end - x_start) * (y_end - y_start) *
                       ((p->colmod == 0x55) ? 2 : 3);
    return esp_lcd_panel_io_tx_color(p->io, CMD_RAMWR, color_data, len);
}

static esp_err_t panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7796_panel_t *p = to_panel(panel);

    /* MX/MY are swapped in meaning once MV (axis exchange) is set, so the
     * caller's x/y are mapped through the current swap state. */
    const bool bit_mx = p->swap_axes ? mirror_y : mirror_x;
    const bool bit_my = p->swap_axes ? mirror_x : mirror_y;

    if (bit_mx) p->madctl |= MADCTL_MX; else p->madctl &= ~MADCTL_MX;
    if (bit_my) p->madctl |= MADCTL_MY; else p->madctl &= ~MADCTL_MY;

    return esp_lcd_panel_io_tx_param(p->io, CMD_MADCTL, &p->madctl, 1);
}

static esp_err_t panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7796_panel_t *p = to_panel(panel);

    p->swap_axes = swap_axes;
    if (swap_axes) p->madctl |= MADCTL_MV; else p->madctl &= ~MADCTL_MV;

    return esp_lcd_panel_io_tx_param(p->io, CMD_MADCTL, &p->madctl, 1);
}

static esp_err_t panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7796_panel_t *p = to_panel(panel);
    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    st7796_panel_t *p = to_panel(panel);
    return esp_lcd_panel_io_tx_param(p->io, invert ? CMD_INVON : CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    st7796_panel_t *p = to_panel(panel);
    return esp_lcd_panel_io_tx_param(p->io, on ? CMD_DISPON : CMD_DISPOFF, NULL, 0);
}

static esp_err_t panel_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    st7796_panel_t *p = to_panel(panel);

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, sleep ? CMD_SLPIN : CMD_SLPOUT,
                                                  NULL, 0), TAG, "sleep");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------ */
esp_err_t lcd_new_panel_st7796(esp_lcd_panel_io_handle_t io,
                               const esp_lcd_panel_dev_config_t *panel_dev,
                               esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev && ret_panel, ESP_ERR_INVALID_ARG,
                        TAG, "null argument");

    st7796_panel_t *p = calloc(1, sizeof(st7796_panel_t));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no memory");

    esp_err_t err = ESP_OK;

    if (panel_dev->reset_gpio_num >= 0) {
        const gpio_config_t cfg = {
            .mode         = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev->reset_gpio_num,
        };
        err = gpio_config(&cfg);
        if (err != ESP_OK) {
            free(p);
            ESP_RETURN_ON_ERROR(err, TAG, "reset gpio");
        }
    }

    switch (panel_dev->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_BGR: p->madctl = MADCTL_BGR; break;
    default:                        p->madctl = 0;          break;
    }

    switch (panel_dev->bits_per_pixel) {
    case 16: p->colmod = 0x55; break;
    case 18: p->colmod = 0x66; break;
    default:
        free(p);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG,
                            "%d bpp is not supported", (int)panel_dev->bits_per_pixel);
    }

    p->io             = io;
    p->reset_gpio_num = panel_dev->reset_gpio_num;
    p->reset_level    = panel_dev->flags.reset_active_high;

    p->base.reset        = panel_reset;
    p->base.init         = panel_init;
    p->base.del          = panel_del;
    p->base.draw_bitmap  = panel_draw_bitmap;
    p->base.mirror       = panel_mirror;
    p->base.swap_xy      = panel_swap_xy;
    p->base.set_gap      = panel_set_gap;
    p->base.invert_color = panel_invert_color;
    p->base.disp_on_off  = panel_disp_on_off;
    p->base.disp_sleep   = panel_disp_sleep;

    *ret_panel = &p->base;
    return ESP_OK;
}
