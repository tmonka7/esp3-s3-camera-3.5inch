#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "esp_camera.h"
#include "img_converters.h"

#include "bsp_camera.h"

#include "app_events.h"
#include "app_settings.h"
#include "app_time.h"
#include "svc_media.h"
#include "svc_notify.h"
#include "svc_detect.h"

static const char *TAG = "svc_detect";

/* The analysis grid. A 1:8 decode of any frame size the UI offers fits;
 * anything larger is cropped, which only costs peripheral sensitivity. */
#define GRID_W          60
#define GRID_H          60
#define GRID_CELLS      (GRID_W * GRID_H)

/* The 1:8 decode target is sized independently of the grid, because the
 * decoder writes the whole reduced frame before we crop it. This covers
 * every sensor mode up to 1024x768; larger modes skip detection rather than
 * overflow the buffer. */
#define DECODE_MAX_W    128
#define DECODE_MAX_H    96
#define DECODE_BYTES    (DECODE_MAX_W * DECODE_MAX_H * 2)

/* The background adapts by 1/16 of the difference per frame: fast enough to
 * absorb a cloud passing, slow enough that a person standing still stays
 * visible for several seconds. */
#define BG_ADAPT_SHIFT  4

/* A blob must appear in this many consecutive frames before it counts. Two
 * frames is enough to reject sensor noise without adding visible latency. */
#define PERSIST_FRAMES  2

/* Blobs smaller than this are noise regardless of sensitivity. */
#define MIN_BLOB_CELLS  6

static bool             s_inited;
static bool             s_enabled;
static SemaphoreHandle_t s_lock;

static uint8_t *s_bg;          /* background luma model  */
static uint8_t *s_cur;         /* current frame luma     */
static uint8_t *s_mask;        /* foreground mask        */
static uint8_t *s_rgb;         /* 1:8 decode scratch     */
static bool     s_bg_ready;

static uint16_t s_grid_w, s_grid_h;

static detect_backend_t s_backend;
static void            *s_backend_ctx;

static detect_event_t s_history[SVC_DETECT_HISTORY];
static size_t         s_hist_count;

static int64_t  s_last_event_us;
static int      s_persist_run;
static uint16_t s_last_cx, s_last_cy;

const char *svc_detect_class_name(detect_class_t cls)
{
    switch (cls) {
    case DETECT_PERSON:  return "Person detected";
    case DETECT_VEHICLE: return "Car detected";
    case DETECT_OBJECT:  return "Object detected";
    default:             return "No detection";
    }
}

/* --------------------------------------------------------------------------
 * History
 * ------------------------------------------------------------------------ */
static void history_push(const detect_event_t *e)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Newest first: shift down and drop the oldest. The list is short and
     * only touched once per detection, so a memmove is cheaper than the
     * bookkeeping a ring buffer would need here. */
    if (s_hist_count < SVC_DETECT_HISTORY) {
        s_hist_count++;
    }
    memmove(&s_history[1], &s_history[0],
            (s_hist_count - 1) * sizeof(detect_event_t));
    s_history[0] = *e;

    xSemaphoreGive(s_lock);
}

size_t svc_detect_history(detect_event_t *out, size_t max)
{
    if (!out || max == 0 || !s_lock) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = (s_hist_count < max) ? s_hist_count : max;
    memcpy(out, s_history, n * sizeof(detect_event_t));
    xSemaphoreGive(s_lock);
    return n;
}

void svc_detect_clear_history(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_hist_count = 0;
    memset(s_history, 0, sizeof(s_history));
    xSemaphoreGive(s_lock);
}

/* --------------------------------------------------------------------------
 * Luma extraction
 * ------------------------------------------------------------------------ */
/** Decodes at 1:8 and reduces RGB565 to luma in the working grid. */
static bool extract_luma(const uint8_t *jpeg, size_t len, uint16_t w, uint16_t h)
{
    const uint16_t src_w = (uint16_t)(w / 8);
    const uint16_t src_h = (uint16_t)(h / 8);

    if (src_w == 0 || src_h == 0) {
        return false;
    }

    /* The decoder writes src_w*src_h pixels regardless of how much of it we
     * go on to use, so refuse a frame that would not fit rather than let it
     * run off the end of the buffer. */
    if (src_w > DECODE_MAX_W || src_h > DECODE_MAX_H) {
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "frame %ux%u is too large to analyse -- "
                          "detection is off until the camera size is lowered", w, h);
        }
        return false;
    }

    if (!jpg2rgb565(jpeg, len, s_rgb, JPG_SCALE_8X)) {
        return false;
    }

    s_grid_w = (src_w > GRID_W) ? GRID_W : src_w;
    s_grid_h = (src_h > GRID_H) ? GRID_H : src_h;

    for (uint16_t y = 0; y < s_grid_h; y++) {
        for (uint16_t x = 0; x < s_grid_w; x++) {
            const size_t   si = ((size_t)y * src_w + x) * 2;
            /* jpg2rgb565 emits big-endian RGB565, matching LV_COLOR_16_SWAP. */
            const uint16_t px = (uint16_t)((s_rgb[si] << 8) | s_rgb[si + 1]);

            const uint8_t r = (uint8_t)(((px >> 11) & 0x1F) << 3);
            const uint8_t g = (uint8_t)(((px >> 5)  & 0x3F) << 2);
            const uint8_t b = (uint8_t)((px & 0x1F) << 3);

            /* Integer Rec.601 luma. */
            s_cur[y * GRID_W + x] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
        }
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Foreground + blob
 * ------------------------------------------------------------------------ */
static int build_mask(uint8_t threshold)
{
    int count = 0;

    for (uint16_t y = 0; y < s_grid_h; y++) {
        for (uint16_t x = 0; x < s_grid_w; x++) {
            const size_t i    = y * GRID_W + x;
            const int    diff = (int)s_cur[i] - (int)s_bg[i];
            const int    mag  = diff < 0 ? -diff : diff;

            if (mag > threshold) {
                s_mask[i] = 1;
                count++;
            } else {
                s_mask[i] = 0;
                /* Only adapt where nothing is happening, so a person who
                 * stops moving is not absorbed into the background. */
                s_bg[i] = (uint8_t)(s_bg[i] + ((diff) >> BG_ADAPT_SHIFT));
            }
        }
    }
    return count;
}

/** Bounding box over the whole foreground mask. */
static bool mask_bounds(uint16_t *bx, uint16_t *by, uint16_t *bw, uint16_t *bh)
{
    uint16_t x0 = GRID_W, y0 = GRID_H, x1 = 0, y1 = 0;
    bool any = false;

    for (uint16_t y = 0; y < s_grid_h; y++) {
        for (uint16_t x = 0; x < s_grid_w; x++) {
            if (!s_mask[y * GRID_W + x]) {
                continue;
            }
            any = true;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x > x1) x1 = x;
            if (y > y1) y1 = y;
        }
    }

    if (!any) {
        return false;
    }
    *bx = x0;
    *by = y0;
    *bw = (uint16_t)(x1 - x0 + 1);
    *bh = (uint16_t)(y1 - y0 + 1);
    return true;
}

/** Geometry-and-motion heuristic. See the header for what this is worth. */
static detect_class_t classify(uint16_t bw, uint16_t bh, uint16_t cx, uint16_t cy,
                               uint8_t *confidence)
{
    const int dx     = (int)cx - (int)s_last_cx;
    const int dy     = (int)cy - (int)s_last_cy;
    const int travel = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

    /* Aspect in eighths to stay in integers: 8 == square. */
    const int aspect = (bw > 0) ? (int)(bh * 8 / bw) : 8;

    if (aspect >= 12 && travel <= 4) {
        *confidence = (uint8_t)(70 + (aspect > 20 ? 20 : aspect));
        return DETECT_PERSON;
    }
    if (aspect <= 5 && travel >= 3) {
        *confidence = (uint8_t)(65 + (travel > 25 ? 25 : travel));
        return DETECT_VEHICLE;
    }

    *confidence = 55;
    return DETECT_OBJECT;
}

/* --------------------------------------------------------------------------
 * Frame handling
 * ------------------------------------------------------------------------ */
static void raise_event(detect_class_t cls, uint8_t confidence,
                        uint16_t bx, uint16_t by, uint16_t bw, uint16_t bh,
                        const uint8_t *jpeg, size_t jpeg_len)
{
    const app_settings_t *cfg = app_settings();

    detect_event_t e = {
        .cls        = cls,
        .confidence = confidence,
        /* Normalise to per mille so the UI can draw the box at any size. */
        .x = (uint16_t)((uint32_t)bx * 1000 / s_grid_w),
        .y = (uint16_t)((uint32_t)by * 1000 / s_grid_h),
        .w = (uint16_t)((uint32_t)bw * 1000 / s_grid_w),
        .h = (uint16_t)((uint32_t)bh * 1000 / s_grid_h),
    };
    app_time_format(e.stamp, sizeof(e.stamp), "%H:%M:%S");

    /* Evidence first: save the very frame that triggered this, not whatever
     * the sensor produces a moment later. */
    char path[96];
    if (svc_media_save_jpeg(jpeg, jpeg_len, path, sizeof(path)) == ESP_OK) {
        strlcpy(e.snapshot, path, sizeof(e.snapshot));
    }

    if (cfg->detect_record_clip) {
        svc_media_record_clip(cfg->clip_seconds);
    }

    history_push(&e);
    app_event_post(APP_EVT_DETECTION, &e, sizeof(e));

    if (cfg->detect_notify) {
        char detail[96];
        snprintf(detail, sizeof(detail), "%s  confidence %u%%", e.stamp, confidence);
        svc_notify_alert(svc_detect_class_name(cls), detail,
                         e.snapshot[0] ? e.snapshot : NULL);
    }

    ESP_LOGI(TAG, "%s (%u%%) at %u,%u %ux%u", svc_detect_class_name(cls),
             confidence, e.x, e.y, e.w, e.h);
}

void svc_detect_submit_frame(const uint8_t *jpeg, size_t len)
{
    if (!s_inited || !s_enabled || !jpeg || !len) {
        return;
    }

    /* Frame geometry comes from the sensor's configured mode rather than
     * from the JPEG, which would mean parsing a SOF0 marker per frame. */
    const framesize_t fs = bsp_camera_get_framesize();
    if (!extract_luma(jpeg, len, resolution[fs].width, resolution[fs].height)) {
        return;
    }

    const app_settings_t *cfg = app_settings();

    /* Sensitivity 1..10 maps to a luma delta of 42 down to 6: a higher
     * setting means a smaller change is enough to trigger. */
    int sens = cfg->detect_sensitivity;
    if (sens < 1)  sens = 1;
    if (sens > 10) sens = 10;
    const uint8_t threshold = (uint8_t)(46 - sens * 4);

    if (!s_bg_ready) {
        memcpy(s_bg, s_cur, GRID_CELLS);
        s_bg_ready = true;
        return;
    }

    const int active = build_mask(threshold);

    uint16_t bx, by, bw, bh;
    if (active < MIN_BLOB_CELLS || !mask_bounds(&bx, &by, &bw, &bh)) {
        s_persist_run = 0;
        return;
    }

    if (++s_persist_run < PERSIST_FRAMES) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (s_last_event_us &&
        (now - s_last_event_us) < (int64_t)cfg->detect_cooldown_s * 1000000) {
        return;     /* still inside the cooldown window */
    }

    const uint16_t cx = (uint16_t)(bx + bw / 2);
    const uint16_t cy = (uint16_t)(by + bh / 2);

    uint8_t        confidence = 0;
    detect_class_t cls;

    if (s_backend) {
        cls = s_backend(jpeg, len, bx, by, bw, bh, &confidence, s_backend_ctx);
    } else {
        cls = classify(bw, bh, cx, cy, &confidence);
    }

    s_last_cx = cx;
    s_last_cy = cy;

    if (cls == DETECT_NONE) {
        return;
    }

    s_last_event_us = now;
    s_persist_run   = 0;
    raise_event(cls, confidence, bx, by, bw, bh, jpeg, len);
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */
static void media_frame_cb(const uint8_t *jpeg, size_t len, void *ctx)
{
    (void)ctx;
    svc_detect_submit_frame(jpeg, len);
}

esp_err_t svc_detect_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    s_bg   = heap_caps_calloc(GRID_CELLS, 1, MALLOC_CAP_SPIRAM);
    s_cur  = heap_caps_calloc(GRID_CELLS, 1, MALLOC_CAP_SPIRAM);
    s_mask = heap_caps_calloc(GRID_CELLS, 1, MALLOC_CAP_SPIRAM);
    /* 1:8 decode target; see DECODE_MAX_W/H. */
    s_rgb  = heap_caps_calloc(DECODE_BYTES, 1, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_bg && s_cur && s_mask && s_rgb, ESP_ERR_NO_MEM,
                        TAG, "work buffers");

    s_inited = true;
    svc_detect_set_enabled(app_settings()->detect_enabled);

    ESP_LOGI(TAG, "detector ready (%s)", s_enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t svc_detect_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        s_bg_ready    = false;   /* relearn the scene when switched back on */
        s_persist_run = 0;
    }

    /* Registering and unregistering the frame callback is what starts and
     * stops the camera pump when nothing else is using it, so a disabled
     * detector costs nothing rather than capturing frames it discards. */
    if (enabled) {
        svc_media_add_frame_cb(media_frame_cb, NULL);
    } else {
        svc_media_remove_frame_cb(media_frame_cb);
    }

    app_settings()->detect_enabled = enabled;
    app_settings_commit_deferred();
    return ESP_OK;
}

bool svc_detect_is_enabled(void)
{
    return s_enabled;
}

void svc_detect_set_backend(detect_backend_t backend, void *ctx)
{
    s_backend_ctx = ctx;
    s_backend     = backend;
}
