#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "lvgl_port.h"
#include "touch_ft6336.h"

static const char *TAG = "lvgl_port";

#define LVGL_TICK_PERIOD_MS   2
/* Upper bound on how long the LVGL task sleeps when idle. lv_timer_handler()
 * says when it next wants to run; this just stops it sleeping forever. */
#define LVGL_MAX_SLEEP_MS     50

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;
static lv_disp_t         *s_disp;

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t      s_lock;
static esp_timer_handle_t     s_tick_timer;

/* --------------------------------------------------------------------------
 * Display
 * ------------------------------------------------------------------------ */
/* Fires on the SPI DMA completion interrupt, which is what lets the LVGL task
 * get on with rendering the next buffer while this one is still going out. */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    (void)io; (void)edata;

    lv_disp_drv_t *drv = user_ctx;
    lv_disp_flush_ready(drv);
    return false;      /* no task woken directly from here */
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px_map)
{
    /* draw_bitmap takes an exclusive end coordinate; LVGL areas are
     * inclusive, hence the +1. */
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              px_map);
    /* flush_ready is called from on_color_trans_done, not here. */
    (void)drv;
}

/* --------------------------------------------------------------------------
 * Touch
 * ------------------------------------------------------------------------ */
static void indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;

    uint16_t x = 0, y = 0;
    if (touch_ft6336_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* --------------------------------------------------------------------------
 * Tick + task
 * ------------------------------------------------------------------------ */
static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "LVGL task running");

    while (true) {
        uint32_t next_ms = LVGL_MAX_SLEEP_MS;

        if (lvgl_port_lock(0)) {
            next_ms = lv_timer_handler();
            lvgl_port_unlock();
        }

        if (next_ms > LVGL_MAX_SLEEP_MS) {
            next_ms = LVGL_MAX_SLEEP_MS;
        }
        /* Never sleep zero ticks: that would spin the core at priority and
         * starve everything else on it. */
        vTaskDelay(pdMS_TO_TICKS(next_ms ? next_ms : 1));
    }
}

/* --------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------ */
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->panel_handle && cfg->io_handle,
                        ESP_ERR_INVALID_ARG, TAG, "bad config");
    if (s_disp) {
        return ESP_OK;
    }

    s_panel = cfg->panel_handle;

    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    lv_init();

    /* Internal RAM, DMA-capable: PSRAM draw buffers would make every flush
     * cross the cache and cost more than the memory saved. */
    const size_t buf_bytes = cfg->buffer_pixels * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG,
                        "draw buffers (%u bytes each)", (unsigned)buf_bytes);

    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, cfg->buffer_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = cfg->hres;
    s_disp_drv.ver_res  = cfg->vres;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = cfg->panel_handle;

    s_disp = lv_disp_drv_register(&s_disp_drv);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "display register");

    const esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(cfg->io_handle,
                                                                  &io_cbs, &s_disp_drv),
                        TAG, "io callbacks");

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.disp    = s_disp;
    s_indev_drv.read_cb = indev_read_cb;
    ESP_RETURN_ON_FALSE(lv_indev_drv_register(&s_indev_drv), ESP_FAIL,
                        TAG, "indev register");

    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name     = "lv_tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &s_tick_timer), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_tick_timer,
                                                 LVGL_TICK_PERIOD_MS * 1000),
                        TAG, "tick start");

    BaseType_t ok;
    if (cfg->task_core >= 0) {
        ok = xTaskCreatePinnedToCore(lvgl_task, "lvgl", cfg->task_stack, NULL,
                                     cfg->task_priority, NULL, cfg->task_core);
    } else {
        ok = xTaskCreate(lvgl_task, "lvgl", cfg->task_stack, NULL,
                         cfg->task_priority, NULL);
    }
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "lvgl task");

    ESP_LOGI(TAG, "LVGL %d.%d.%d up at %ux%u, %u-pixel double buffer",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             (unsigned)cfg->hres, (unsigned)cfg->vres, (unsigned)cfg->buffer_pixels);
    return ESP_OK;
}

lv_disp_t *lvgl_port_disp(void)
{
    return s_disp;
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    if (!s_lock) {
        return false;
    }
    const TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY
                                               : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    if (s_lock) {
        xSemaphoreGiveRecursive(s_lock);
    }
}
