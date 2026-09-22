/* Camera frame pump, snapshots and recording.
 *
 * One task pulls JPEG frames off the sensor and fans them out:
 *   1. the recorder appends them to an AVI verbatim,
 *   2. the detector gets the raw JPEG to decode at 1:8,
 *   3. the live view gets an RGB565 decode into a back buffer.
 *
 * Consumers are registered as callbacks so this component never references
 * LVGL, and the preview buffer is double-buffered: the pump decodes into the
 * back buffer, then swaps under the caller's lock.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Preview never exceeds the panel, so one allocation covers every mode. */
#define MEDIA_PREVIEW_MAX_W   480
#define MEDIA_PREVIEW_MAX_H   320

typedef enum {
    MEDIA_REC_IDLE = 0,
    MEDIA_REC_RECORDING,
    MEDIA_REC_ERROR,
} media_record_state_t;

/** Payload of APP_EVT_RECORD_STATE. */
typedef struct {
    media_record_state_t state;
    uint32_t             seconds;
    uint32_t             frames;
    uint32_t             kbytes;
    char                 path[72];
} media_record_status_t;

/**
 * Called from the pump task once a frame has been decoded, with the front
 * buffer. Implementations must copy nothing: take the UI lock, point the
 * image widget at `rgb565`, invalidate, unlock.
 */
typedef void (*media_preview_cb_t)(const uint8_t *rgb565, int w, int h, void *ctx);

/** Called from the pump task with the untouched JPEG, for the detector. */
typedef void (*media_frame_cb_t)(const uint8_t *jpeg, size_t len, void *ctx);

esp_err_t svc_media_init(void);

/* ---- live view -------------------------------------------------------- */
esp_err_t svc_media_start_preview(void);
esp_err_t svc_media_stop_preview(void);
bool      svc_media_preview_active(void);
void      svc_media_set_preview_cb(media_preview_cb_t cb, void *ctx);
void      svc_media_set_frame_cb(media_frame_cb_t cb, void *ctx);

/** Measured preview frame rate, for the status strip. */
int svc_media_preview_fps(void);

/* ---- stills ----------------------------------------------------------- */

/** Captures a fresh frame, writes it to /sdcard/image, returns its path. */
esp_err_t svc_media_snapshot(char *out_path, size_t out_path_len);

/**
 * Writes a JPEG the caller already holds. The detector uses this so its
 * evidence still is the frame that triggered it, rather than whatever the
 * sensor produced a moment later.
 */
esp_err_t svc_media_save_jpeg(const uint8_t *jpeg, size_t len,
                              char *out_path, size_t out_path_len);

/* ---- recording -------------------------------------------------------- */
esp_err_t svc_media_record_start(void);
esp_err_t svc_media_record_stop(void);
bool      svc_media_recording(void);
void      svc_media_record_status(media_record_status_t *out);

/**
 * Records a fixed-length clip and stops by itself. Used by the detector when
 * "record on detection" is enabled; a no-op if a recording is already running.
 */
esp_err_t svc_media_record_clip(uint16_t seconds);

/**
 * Deletes the oldest recordings until the card has at least
 * `min_free_percent` free. Called before each recording starts.
 */
esp_err_t svc_media_reclaim_space(void);

#ifdef __cplusplus
}
#endif
