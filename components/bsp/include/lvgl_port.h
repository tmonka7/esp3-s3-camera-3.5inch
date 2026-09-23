/* LVGL integration.
 *
 * In-tree rather than `espressif/esp_lvgl_port`, so the project builds
 * offline and is not coupled to that component's LVGL version policy.
 *
 * Provides the four things LVGL needs on this board: draw buffers, a flush
 * callback onto the esp_lcd panel, a touch input device, and a tick source --
 * plus the recursive lock that lets other tasks touch widgets safely.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t    panel_handle;

    uint32_t hres;              /* resolution AFTER rotation */
    uint32_t vres;

    /* Pixels per draw buffer. Two are allocated, in DMA-capable internal
     * RAM, so one can be flushed while the next is rendered. */
    uint32_t buffer_pixels;

    int      task_priority;
    int      task_stack;
    int      task_core;         /* -1 for no affinity */
} lvgl_port_cfg_t;

/** Calls lv_init(), creates the display, the input device and the LVGL task. */
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg);

lv_disp_t *lvgl_port_disp(void);

/**
 * Takes the LVGL mutex. It is recursive, so nesting is safe.
 *
 * @param timeout_ms 0 waits forever.
 */
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif
