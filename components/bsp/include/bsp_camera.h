#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The sensor runs in JPEG mode permanently. One encoded frame then feeds all
 * three consumers without a mode switch:
 *   - the live view, decoded to RGB565 at display size,
 *   - the AVI recorder, which stores it verbatim,
 *   - the detector, decoded 1:8 to a tiny luma map.
 */
esp_err_t bsp_camera_init(void);
esp_err_t bsp_camera_deinit(void);
bool      bsp_camera_is_ready(void);

/** Underlying sensor handle, for exposure/white-balance tweaks. */
sensor_t *bsp_camera_sensor(void);

/** Switches the capture resolution at runtime (esp32-camera framesize_t). */
esp_err_t bsp_camera_set_framesize(framesize_t size);
framesize_t bsp_camera_get_framesize(void);

/** JPEG quality, 10 (best) .. 63 (worst). */
esp_err_t bsp_camera_set_quality(int quality);

/** Horizontal / vertical flip, persisted by the settings service. */
esp_err_t bsp_camera_set_flip(bool hmirror, bool vflip);

#ifdef __cplusplus
}
#endif
