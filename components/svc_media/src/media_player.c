#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "img_converters.h"

#include "media_avi.h"
#include "media_player.h"
#include "svc_media.h"

static const char *TAG = "player";

#define PREVIEW_BUF_BYTES  (MEDIA_PREVIEW_MAX_W * MEDIA_PREVIEW_MAX_H * 2)

static avi_reader_t        s_reader;
static player_state_t      s_state;
static uint32_t            s_frame;
static bool                s_running;
static SemaphoreHandle_t   s_lock;
static media_preview_cb_t  s_cb;
static void               *s_cb_ctx;

/* Same double-buffer discipline as the live preview: decode into the back
 * buffer, publish it, and only then reuse the other one. */
static uint8_t *s_rgb[2];
static int      s_back;
static uint8_t *s_jpeg;
static size_t   s_jpeg_cap;

/** Seek requested by the UI; -1 means "none". Applied by the player task so
 *  the file position is only ever touched from one place. */
static int32_t s_seek_to = -1;

static void deliver_frame(uint32_t n)
{
    size_t len = 0;
    if (avi_reader_frame(&s_reader, n, s_jpeg, s_jpeg_cap, &len) != ESP_OK) {
        ESP_LOGW(TAG, "cannot read frame %lu", (unsigned long)n);
        return;
    }

    uint8_t *back = s_rgb[s_back];
    if (!jpg2rgb565(s_jpeg, len, back, JPG_SCALE_NONE)) {
        ESP_LOGW(TAG, "frame %lu failed to decode", (unsigned long)n);
        return;
    }

    s_back = 1 - s_back;
    if (s_cb) {
        s_cb(back, s_reader.width, s_reader.height, s_cb_ctx);
    }
}

static void player_task(void *arg)
{
    (void)arg;

    while (s_running) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (!s_reader.open) {
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* A seek repaints even while paused, so scrubbing feels live. */
        if (s_seek_to >= 0) {
            s_frame   = (uint32_t)s_seek_to;
            s_seek_to = -1;
            deliver_frame(s_frame);
            xSemaphoreGive(s_lock);
            continue;
        }

        if (s_state != PLAYER_PLAYING) {
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        const int64_t started = esp_timer_get_time();
        deliver_frame(s_frame);

        if (++s_frame >= s_reader.frame_count) {
            s_frame = 0;
            s_state = PLAYER_PAUSED;      /* hold on the last frame */
            ESP_LOGI(TAG, "playback finished");
        }

        const uint32_t period_us = s_reader.us_per_frame;
        xSemaphoreGive(s_lock);

        /* Pace against decode time so playback keeps the recorded rate
         * instead of drifting slower by however long the decode took. */
        const int64_t spent = esp_timer_get_time() - started;
        const int64_t wait  = (int64_t)period_us - spent;
        vTaskDelay(wait > 0 ? pdMS_TO_TICKS((uint32_t)(wait / 1000)) : 1);
    }

    vTaskDelete(NULL);
}

esp_err_t media_player_init(void)
{
    if (s_running) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    for (int i = 0; i < 2; i++) {
        s_rgb[i] = heap_caps_malloc(PREVIEW_BUF_BYTES, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_rgb[i], ESP_ERR_NO_MEM, TAG, "rgb buffer");
        memset(s_rgb[i], 0, PREVIEW_BUF_BYTES);
    }

    s_running = true;
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(player_task, "player", 6144, NULL, 4,
                                                NULL, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

esp_err_t media_player_open(const char *path)
{
    ESP_RETURN_ON_FALSE(path, ESP_ERR_INVALID_ARG, TAG, "null");
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_reader.open) {
        avi_reader_close(&s_reader);
    }

    esp_err_t err = avi_reader_open(&s_reader, path);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }

    if (s_reader.width > MEDIA_PREVIEW_MAX_W || s_reader.height > MEDIA_PREVIEW_MAX_H) {
        ESP_LOGE(TAG, "%ux%u exceeds the decode buffer", s_reader.width, s_reader.height);
        avi_reader_close(&s_reader);
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Size the JPEG staging buffer once, from the file's own largest frame. */
    const size_t needed = avi_reader_max_frame_bytes(&s_reader) + 64;
    if (needed > s_jpeg_cap) {
        heap_caps_free(s_jpeg);
        s_jpeg = heap_caps_malloc(needed, MALLOC_CAP_SPIRAM);
        if (!s_jpeg) {
            s_jpeg_cap = 0;
            avi_reader_close(&s_reader);
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
        s_jpeg_cap = needed;
    }

    s_frame   = 0;
    s_seek_to = -1;
    s_state   = PLAYER_PLAYING;

    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "playing %s", path);
    return ESP_OK;
}

esp_err_t media_player_play(void)
{
    ESP_RETURN_ON_FALSE(s_reader.open, ESP_ERR_INVALID_STATE, TAG, "nothing open");
    s_state = PLAYER_PLAYING;
    return ESP_OK;
}

esp_err_t media_player_pause(void)
{
    ESP_RETURN_ON_FALSE(s_reader.open, ESP_ERR_INVALID_STATE, TAG, "nothing open");
    s_state = PLAYER_PAUSED;
    return ESP_OK;
}

esp_err_t media_player_toggle(void)
{
    return (s_state == PLAYER_PLAYING) ? media_player_pause() : media_player_play();
}

esp_err_t media_player_stop(void)
{
    if (!s_lock) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_reader.open) {
        avi_reader_close(&s_reader);
    }
    s_state = PLAYER_STOPPED;
    s_frame = 0;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t media_player_seek_permille(uint16_t pos)
{
    ESP_RETURN_ON_FALSE(s_reader.open, ESP_ERR_INVALID_STATE, TAG, "nothing open");

    if (pos > 1000) {
        pos = 1000;
    }
    uint32_t target = (uint32_t)(((uint64_t)s_reader.frame_count * pos) / 1000u);
    if (target >= s_reader.frame_count) {
        target = s_reader.frame_count - 1;
    }
    s_seek_to = (int32_t)target;
    return ESP_OK;
}

esp_err_t media_player_step(int delta)
{
    ESP_RETURN_ON_FALSE(s_reader.open, ESP_ERR_INVALID_STATE, TAG, "nothing open");

    int64_t target = (int64_t)s_frame + delta;
    if (target < 0) {
        target = 0;
    }
    if (target >= (int64_t)s_reader.frame_count) {
        target = (int64_t)s_reader.frame_count - 1;
    }

    s_state   = PLAYER_PAUSED;
    s_seek_to = (int32_t)target;
    return ESP_OK;
}

void media_player_status(player_status_t *out)
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->state = s_state;

    if (!s_reader.open) {
        return;
    }

    const uint32_t fps = s_reader.us_per_frame ? (1000000u / s_reader.us_per_frame) : 25;

    out->frame       = s_frame;
    out->frame_count = s_reader.frame_count;
    out->width       = s_reader.width;
    out->height      = s_reader.height;
    out->duration_s  = fps ? (s_reader.frame_count / fps) : 0;
    out->position_s  = fps ? (s_frame / fps) : 0;
}

void media_player_set_frame_cb(media_preview_cb_t cb, void *ctx)
{
    s_cb_ctx = ctx;
    s_cb     = cb;
}
