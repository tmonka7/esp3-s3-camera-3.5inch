#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "esp_camera.h"
#include "img_converters.h"

#include "bsp_board.h"
#include "bsp_camera.h"
#include "bsp_storage.h"
#include "app_events.h"
#include "app_settings.h"
#include "app_time.h"
#include "media_avi.h"
#include "svc_media.h"

static const char *TAG = "svc_media";

#define PREVIEW_BUF_BYTES  (MEDIA_PREVIEW_MAX_W * MEDIA_PREVIEW_MAX_H * 2)

static bool               s_running;
static bool               s_preview_on;
static media_preview_cb_t s_preview_cb;
static void              *s_preview_ctx;
static media_frame_cb_t   s_frame_cb;
static void              *s_frame_ctx;

/* Two buffers: the pump decodes into `back`, then publishes it as `front`.
 * The UI only ever reads `front`, and only while it holds the LVGL lock --
 * which is also when the pump is blocked inside the preview callback. */
static uint8_t *s_buf[2];
static int      s_back;

static int      s_fps;
static uint32_t s_frames_since_tick;
static int64_t  s_fps_tick_us;

/* ---- recording state -------------------------------------------------- */
static avi_writer_t      s_avi;
static bool              s_rec_active;
static uint16_t          s_rec_limit_s;     /* 0 = until stopped */
static SemaphoreHandle_t s_rec_lock;
static media_record_status_t s_rec_status;

/* --------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */
static void publish_record_state(void)
{
    app_event_post(APP_EVT_RECORD_STATE, &s_rec_status, sizeof(s_rec_status));
}

/** Decode scale that keeps a frame inside the preview buffer. */
static jpg_scale_t scale_for(uint16_t w, uint16_t h)
{
    if (w <= MEDIA_PREVIEW_MAX_W && h <= MEDIA_PREVIEW_MAX_H) return JPG_SCALE_NONE;
    if (w / 2 <= MEDIA_PREVIEW_MAX_W && h / 2 <= MEDIA_PREVIEW_MAX_H) return JPG_SCALE_2X;
    if (w / 4 <= MEDIA_PREVIEW_MAX_W && h / 4 <= MEDIA_PREVIEW_MAX_H) return JPG_SCALE_4X;
    return JPG_SCALE_8X;
}

static int scale_divisor(jpg_scale_t s)
{
    switch (s) {
    case JPG_SCALE_2X: return 2;
    case JPG_SCALE_4X: return 4;
    case JPG_SCALE_8X: return 8;
    default:           return 1;
    }
}

/* --------------------------------------------------------------------------
 * Recording
 * ------------------------------------------------------------------------ */
static void record_append(const camera_fb_t *fb)
{
    if (xSemaphoreTake(s_rec_lock, 0) != pdTRUE) {
        return;
    }

    if (s_rec_active) {
        if (avi_writer_add_frame(&s_avi, fb->buf, fb->len) != ESP_OK) {
            ESP_LOGE(TAG, "frame write failed -- stopping recording");
            avi_writer_close(&s_avi);
            s_rec_active        = false;
            s_rec_status.state  = MEDIA_REC_ERROR;
            publish_record_state();
        } else {
            s_rec_status.seconds = avi_writer_elapsed_s(&s_avi);
            s_rec_status.frames  = s_avi.frame_count;
            s_rec_status.kbytes  = s_avi.total_bytes / 1024;

            /* Auto-stop for fixed-length clips. */
            if (s_rec_limit_s && s_rec_status.seconds >= s_rec_limit_s) {
                avi_writer_close(&s_avi);
                s_rec_active       = false;
                s_rec_limit_s      = 0;
                s_rec_status.state = MEDIA_REC_IDLE;
                ESP_LOGI(TAG, "clip finished: %s", s_rec_status.path);
            }
            publish_record_state();
        }
    }

    xSemaphoreGive(s_rec_lock);
}

/* --------------------------------------------------------------------------
 * Frame pump
 * ------------------------------------------------------------------------ */
static void pump_task(void *arg)
{
    (void)arg;

    while (s_running) {
        /* The pump runs for any consumer, not just the live view: the
         * detector registers a frame callback and needs frames whether or
         * not a screen happens to be showing the camera. */
        if (!s_preview_on && !s_rec_active && !s_frame_cb) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "no frame");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            /* The sensor is configured for JPEG at init; anything else means
             * someone changed it behind our back. */
            ESP_LOGE(TAG, "unexpected pixel format %d", (int)fb->format);
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        record_append(fb);

        if (s_frame_cb) {
            s_frame_cb(fb->buf, fb->len, s_frame_ctx);
        }

        if (s_preview_on && s_preview_cb) {
            const jpg_scale_t scale = scale_for(fb->width, fb->height);
            const int         div   = scale_divisor(scale);
            const int         w     = fb->width / div;
            const int         h     = fb->height / div;

            uint8_t *back = s_buf[s_back];
            if (jpg2rgb565(fb->buf, fb->len, back, scale)) {
                s_back = 1 - s_back;                 /* back becomes front  */
                s_preview_cb(back, w, h, s_preview_ctx);

                s_frames_since_tick++;
                const int64_t now = esp_timer_get_time();
                if (now - s_fps_tick_us >= 1000000) {
                    s_fps = (int)s_frames_since_tick;
                    s_frames_since_tick = 0;
                    s_fps_tick_us = now;
                }
            }
        }

        esp_camera_fb_return(fb);

        /* Yield so the LVGL and event tasks on core 0 are never starved even
         * if the sensor has a frame waiting every single time. */
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */
esp_err_t svc_media_init(void)
{
    if (s_running) {
        return ESP_OK;
    }

    s_rec_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rec_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(PREVIEW_BUF_BYTES, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_buf[i], ESP_ERR_NO_MEM, TAG, "preview buffer %d", i);
        memset(s_buf[i], 0, PREVIEW_BUF_BYTES);
    }

    s_running = true;
    /* Core 1: JPEG decode is the heaviest periodic work in the system and
     * core 0 is already carrying LVGL, the event loop and the pollers. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(pump_task, "cam_pump", 6144, NULL, 5,
                                                NULL, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "pump task");

    ESP_LOGI(TAG, "media service started");
    return ESP_OK;
}

esp_err_t svc_media_start_preview(void)
{
    ESP_RETURN_ON_FALSE(bsp_camera_is_ready(), ESP_ERR_INVALID_STATE, TAG, "no camera");
    s_preview_on        = true;
    s_fps_tick_us       = esp_timer_get_time();
    s_frames_since_tick = 0;
    return ESP_OK;
}

esp_err_t svc_media_stop_preview(void)
{
    s_preview_on = false;
    s_fps        = 0;
    return ESP_OK;
}

bool svc_media_preview_active(void)
{
    return s_preview_on;
}

void svc_media_set_preview_cb(media_preview_cb_t cb, void *ctx)
{
    s_preview_ctx = ctx;
    s_preview_cb  = cb;       /* set last: the pump reads cb before ctx */
}

void svc_media_set_frame_cb(media_frame_cb_t cb, void *ctx)
{
    s_frame_ctx = ctx;
    s_frame_cb  = cb;
}

int svc_media_preview_fps(void)
{
    return s_fps;
}

/* --------------------------------------------------------------------------
 * Stills
 * ------------------------------------------------------------------------ */
esp_err_t svc_media_save_jpeg(const uint8_t *jpeg, size_t len,
                              char *out_path, size_t out_path_len)
{
    ESP_RETURN_ON_FALSE(jpeg && len, ESP_ERR_INVALID_ARG, TAG, "empty frame");
    ESP_RETURN_ON_FALSE(bsp_storage_mounted(), ESP_ERR_NOT_FOUND, TAG, "no TF card");

    char stamp[24];
    app_time_stamp_filename(stamp, sizeof(stamp));

    char path[96];
    snprintf(path, sizeof(path), BSP_SD_MOUNT_POINT "/image/%s.jpg", stamp);

    FILE *f = fopen(path, "wb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "cannot create %s", path);

    const bool ok = (fwrite(jpeg, 1, len, f) == len);
    fclose(f);

    if (!ok) {
        ESP_LOGE(TAG, "snapshot write failed: %s", path);
        unlink(path);          /* no truncated stills left behind */
        return ESP_FAIL;
    }

    if (out_path && out_path_len) {
        strlcpy(out_path, path, out_path_len);
    }
    ESP_LOGI(TAG, "snapshot %s (%u bytes)", path, (unsigned)len);
    return ESP_OK;
}

esp_err_t svc_media_snapshot(char *out_path, size_t out_path_len)
{
    ESP_RETURN_ON_FALSE(bsp_camera_is_ready(), ESP_ERR_INVALID_STATE, TAG, "no camera");

    camera_fb_t *fb = esp_camera_fb_get();
    ESP_RETURN_ON_FALSE(fb, ESP_FAIL, TAG, "capture failed");

    const esp_err_t err = svc_media_save_jpeg(fb->buf, fb->len, out_path, out_path_len);
    esp_camera_fb_return(fb);
    return err;
}

/* --------------------------------------------------------------------------
 * Storage housekeeping
 * ------------------------------------------------------------------------ */
/** Finds the oldest regular file under `dir`. Returns false when empty. */
static bool oldest_file(const char *dir, char *out, size_t out_len, time_t *out_mtime)
{
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }

    bool    found  = false;
    time_t  oldest = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }

        char path[256];
        size_t used = strlcpy(path, dir, sizeof(path));
        if (used >= sizeof(path)) {
            ESP_LOGW(TAG, "directory path too long while scanning %s", dir);
            continue;
        }
        if (strlcat(path, "/", sizeof(path)) >= sizeof(path)) {
            ESP_LOGW(TAG, "path too long while scanning %s", dir);
            continue;
        }
        if (strlcat(path, e->d_name, sizeof(path)) >= sizeof(path)) {
            ESP_LOGW(TAG, "filename too long while scanning %s: %s", dir, e->d_name);
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        if (!found || st.st_mtime < oldest) {
            found  = true;
            oldest = st.st_mtime;
            strlcpy(out, path, out_len);
        }
    }

    closedir(d);
    if (found && out_mtime) {
        *out_mtime = oldest;
    }
    return found;
}

esp_err_t svc_media_reclaim_space(void)
{
    const app_settings_t *cfg = app_settings();
    if (!cfg->ring_delete_oldest || !bsp_storage_mounted()) {
        return ESP_OK;
    }

    /* Bounded so a pathological card cannot spin here forever. */
    for (int guard = 0; guard < 64; guard++) {
        bsp_storage_info_t info;
        if (bsp_storage_info(&info) != ESP_OK || info.total_bytes == 0) {
            return ESP_FAIL;
        }

        const uint64_t free_bytes = info.total_bytes - info.used_bytes;
        const int free_pct = (int)((free_bytes * 100) / info.total_bytes);
        if (free_pct >= cfg->min_free_percent) {
            return ESP_OK;
        }

        /* Video first: clips dwarf stills, so one delete usually suffices. */
        char   path[160];
        time_t mtime = 0;
        if (!oldest_file(BSP_SD_MOUNT_POINT "/video", path, sizeof(path), &mtime) &&
            !oldest_file(BSP_SD_MOUNT_POINT "/image", path, sizeof(path), &mtime)) {
            ESP_LOGW(TAG, "card is %d%% free but there is nothing left to delete", free_pct);
            return ESP_ERR_NOT_FOUND;
        }

        ESP_LOGW(TAG, "reclaiming space: deleting %s", path);
        if (unlink(path) != 0) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Recording
 * ------------------------------------------------------------------------ */
esp_err_t svc_media_record_start(void)
{
    ESP_RETURN_ON_FALSE(bsp_camera_is_ready(), ESP_ERR_INVALID_STATE, TAG, "no camera");
    ESP_RETURN_ON_FALSE(bsp_storage_mounted(), ESP_ERR_NOT_FOUND, TAG, "no TF card");

    if (xSemaphoreTake(s_rec_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_rec_active) {
        xSemaphoreGive(s_rec_lock);
        return ESP_OK;
    }

    svc_media_reclaim_space();

    char stamp[24];
    app_time_stamp_filename(stamp, sizeof(stamp));

    char path[96];
    snprintf(path, sizeof(path), BSP_SD_MOUNT_POINT "/video/%s.avi", stamp);

    /* Record at whatever the sensor is producing; the AVI header carries the
     * real geometry so playback does not need to guess. */
    const framesize_t fs = bsp_camera_get_framesize();
    const uint16_t    w  = resolution[fs].width;
    const uint16_t    h  = resolution[fs].height;

    const esp_err_t err = avi_writer_open(&s_avi, path, w, h);
    if (err != ESP_OK) {
        s_rec_status.state = MEDIA_REC_ERROR;
        xSemaphoreGive(s_rec_lock);
        publish_record_state();
        return err;
    }

    s_rec_active           = true;
    s_rec_status.state     = MEDIA_REC_RECORDING;
    s_rec_status.seconds   = 0;
    s_rec_status.frames    = 0;
    s_rec_status.kbytes    = 0;
    strlcpy(s_rec_status.path, path, sizeof(s_rec_status.path));

    xSemaphoreGive(s_rec_lock);
    publish_record_state();
    return ESP_OK;
}

esp_err_t svc_media_record_stop(void)
{
    if (xSemaphoreTake(s_rec_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    if (s_rec_active) {
        err = avi_writer_close(&s_avi);
        s_rec_active  = false;
        s_rec_limit_s = 0;
        s_rec_status.state = (err == ESP_OK) ? MEDIA_REC_IDLE : MEDIA_REC_ERROR;
    }

    xSemaphoreGive(s_rec_lock);
    publish_record_state();
    return err;
}

esp_err_t svc_media_record_clip(uint16_t seconds)
{
    if (s_rec_active) {
        return ESP_OK;      /* already capturing; do not cut it short */
    }
    const esp_err_t err = svc_media_record_start();
    if (err == ESP_OK) {
        s_rec_limit_s = seconds;
    }
    return err;
}

bool svc_media_recording(void)
{
    return s_rec_active;
}

void svc_media_record_status(media_record_status_t *out)
{
    if (out) {
        *out = s_rec_status;
    }
}
