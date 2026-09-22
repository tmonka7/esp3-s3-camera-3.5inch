/* External person / object detection.
 *
 * WHAT THIS DOES ON-DEVICE
 * ------------------------
 * Every camera frame is decoded at 1:8 into a small luma map and compared
 * against a slowly-adapting background. Connected foreground cells are
 * grouped into blobs, and a blob that persists across frames becomes a
 * detection. A blob is then labelled from its geometry and motion:
 *
 *   tall and narrow, moving slowly   -> PERSON
 *   wide and low, moving quickly     -> VEHICLE
 *   anything else                    -> OBJECT
 *
 * That geometry rule is a heuristic, not a trained classifier. It is
 * dependable for "something is there and it is moving", which is what drives
 * the alert, the clip and the webhook -- but it will mislabel an umbrella as
 * a person often enough that you should not build a security policy on the
 * label alone.
 *
 * For real classification, register an esp-dl backend with
 * svc_detect_set_backend(); the pipeline below then only supplies motion
 * gating so the model runs on frames that contain something.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SVC_DETECT_HISTORY   32

typedef enum {
    DETECT_NONE = 0,
    DETECT_PERSON,
    DETECT_VEHICLE,
    DETECT_OBJECT,
} detect_class_t;

/** Payload of APP_EVT_DETECTION. Box coordinates are per mille of frame. */
typedef struct {
    detect_class_t cls;
    uint8_t        confidence;     /* 0..100 */
    uint16_t       x, y, w, h;
    char           stamp[16];      /* "10:24:36" */
    char           snapshot[72];   /* empty when nothing was saved */
} detect_event_t;

/**
 * Optional replacement classifier, e.g. an esp-dl model. Receives the JPEG
 * frame plus the motion box the cheap pipeline found, and returns a class.
 * Returning DETECT_NONE suppresses the event entirely.
 */
typedef detect_class_t (*detect_backend_t)(const uint8_t *jpeg, size_t len,
                                           uint16_t x, uint16_t y,
                                           uint16_t w, uint16_t h,
                                           uint8_t *out_confidence, void *ctx);

esp_err_t svc_detect_init(void);

/** Detection follows settings.detect_enabled; this forces it either way. */
esp_err_t svc_detect_set_enabled(bool enabled);
bool      svc_detect_is_enabled(void);

void svc_detect_set_backend(detect_backend_t backend, void *ctx);

/** Most recent events, newest first. Returns how many were written. */
size_t svc_detect_history(detect_event_t *out, size_t max);

void svc_detect_clear_history(void);

const char *svc_detect_class_name(detect_class_t cls);

/** Feeds one JPEG frame. Wired to the media service's frame callback. */
void svc_detect_submit_frame(const uint8_t *jpeg, size_t len);

#ifdef __cplusplus
}
#endif
