#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_camera.h"
#include "img_converters.h"

#include "app_events.h"
#include "app_settings.h"
#include "bsp_camera.h"
#include "bsp_storage.h"
#include "face_lbp.h"
#include "face_locate.h"
#include "svc_access.h"
#include "svc_face.h"
#include "svc_media.h"

static const char *TAG = "svc_face";

/* Analysis resolution. Large enough that a face at arm's length still has
 * detail once it is cropped to 64x64, small enough that a decode fits in a
 * fraction of the frame interval. */
#define ANALYSIS_MAX_W      320
#define ANALYSIS_MAX_H      240
#define ANALYSIS_BYTES      (ANALYSIS_MAX_W * ANALYSIS_MAX_H * 2)

/* A VGA JPEG at the quality this firmware uses runs 30-60 KB. Frames larger
 * than this are skipped rather than truncated. */
#define JPEG_HOLD_BYTES     (96 * 1024)

#define RECOGNISE_PERIOD_MS 500     /* ~2 Hz is plenty for a door   */
#define ENROL_PERIOD_MS     400     /* spacing between reference shots */
#define ENROL_TIMEOUT_S     45
#define SUBMIT_COOLDOWN_MS  4000    /* gap between unlock attempts  */

/* Reject a match that is not clearly better than the runner-up. Two people
 * who both score mediocre is exactly the shape of a false accept. */
#define AMBIGUITY_RATIO     0.80f

#define DB_PATH             BSP_SD_MOUNT_POINT "/faces/db.bin"
#define DB_MAGIC            0x45434146u     /* "FACE" little-endian */
#define DB_VERSION          1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t samples_per;
    uint16_t desc_len;
    uint16_t subject_size;
    uint16_t next_id;
} db_header_t;

/* ---- state ------------------------------------------------------------ */
static bool              s_ready;
static bool              s_enabled;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_work;        /* pump -> worker handoff */

static uint8_t  *s_jpeg;                /* holding copy of one frame */
static size_t    s_jpeg_len;
/* Written by the worker, read by the pump task -- the handoff depends on
 * this being re-read rather than cached in a register. */
static volatile bool s_jpeg_busy;

static uint8_t  *s_rgb;                 /* decoded analysis frame */
static uint8_t  *s_templates;           /* subjects x samples x desc */
static uint8_t   s_gray[FACE_NORM_W * FACE_NORM_H];
static uint8_t   s_desc[FACE_DESC_LEN];

static face_subject_t s_subjects[FACE_MAX_SUBJECTS];
static uint16_t       s_count;
static uint16_t       s_next_id = 1;

static face_status_t s_status;
static int64_t       s_last_run_us;
static int64_t       s_last_submit_us;

/* ---- enrolment -------------------------------------------------------- */
static volatile bool s_enrolling;   /* read from the pump task too */
static char     s_enrol_name[FACE_NAME_LEN];
static uint8_t  s_enrol_got;
static int64_t  s_enrol_until_us;
static int64_t  s_enrol_last_us;
static uint8_t *s_enrol_buf;            /* samples x desc, staged here */

static uint8_t *template_at(int subject, int sample)
{
    return s_templates +
           ((size_t)subject * FACE_SAMPLES_PER + sample) * FACE_DESC_LEN;
}

/* --------------------------------------------------------------------------
 * Persistence
 *
 * The face database lives on the TF card, not in NVS: the nvs partition is
 * 24 KB and one person's templates are nearly 5 KB, so it would never fit.
 * ------------------------------------------------------------------------ */
static esp_err_t db_save(void)
{
    if (!bsp_storage_mounted()) {
        ESP_LOGW(TAG, "no card -- enrolled faces will not survive a reboot");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(DB_PATH, "wb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "cannot write %s", DB_PATH);

    const db_header_t h = {
        .magic        = DB_MAGIC,
        .version      = DB_VERSION,
        .count        = s_count,
        .samples_per  = FACE_SAMPLES_PER,
        .desc_len     = FACE_DESC_LEN,
        .subject_size = (uint16_t)sizeof(face_subject_t),
        .next_id      = s_next_id,
    };

    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    if (ok && s_count) {
        ok = fwrite(s_subjects, sizeof(s_subjects[0]), s_count, f) == s_count;
    }
    if (ok && s_count) {
        const size_t n = (size_t)s_count * FACE_SAMPLES_PER;
        ok = fwrite(s_templates, FACE_DESC_LEN, n, f) == n;
    }
    fclose(f);

    if (!ok) {
        ESP_LOGE(TAG, "short write to %s", DB_PATH);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved %u face(s)", (unsigned)s_count);
    return ESP_OK;
}

static void db_load(void)
{
    s_count   = 0;
    s_next_id = 1;

    if (!bsp_storage_mounted()) {
        return;
    }

    FILE *f = fopen(DB_PATH, "rb");
    if (!f) {
        ESP_LOGI(TAG, "no face database yet");
        return;
    }

    db_header_t h;
    if (fread(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        return;
    }

    /* Any change to the descriptor geometry makes stored templates
     * meaningless, so they are discarded rather than misread -- the same
     * version gate the settings blob uses. */
    if (h.magic != DB_MAGIC || h.version != DB_VERSION ||
        h.samples_per != FACE_SAMPLES_PER || h.desc_len != FACE_DESC_LEN ||
        h.subject_size != sizeof(face_subject_t) ||
        h.count > FACE_MAX_SUBJECTS) {
        ESP_LOGW(TAG, "face database is from a different build -- ignoring it");
        fclose(f);
        return;
    }

    if (h.count &&
        fread(s_subjects, sizeof(s_subjects[0]), h.count, f) != h.count) {
        fclose(f);
        return;
    }

    const size_t n = (size_t)h.count * FACE_SAMPLES_PER;
    if (n && fread(s_templates, FACE_DESC_LEN, n, f) != n) {
        fclose(f);
        return;
    }
    fclose(f);

    s_count   = h.count;
    s_next_id = h.next_id ? h.next_id : 1;
    ESP_LOGI(TAG, "%u face(s) loaded", (unsigned)s_count);
}

/* --------------------------------------------------------------------------
 * Matching
 * ------------------------------------------------------------------------ */
/** Caller holds s_lock. Returns the subject index or -1. */
static int match(const uint8_t *desc, uint8_t *out_confidence)
{
    int   best_i = -1;
    float best_d = 9.0f;
    float second_d = 9.0f;

    for (int i = 0; i < s_count; i++) {
        /* A person's best shot decides -- the samples deliberately differ, so
         * averaging them would just blur the one that actually matches. */
        float d = 9.0f;
        for (int s = 0; s < s_subjects[i].samples && s < FACE_SAMPLES_PER; s++) {
            const float sd = face_lbp_distance(desc, template_at(i, s));
            if (sd < d) {
                d = sd;
            }
        }

        if (d < best_d) {
            second_d = best_d;
            best_d   = d;
            best_i   = i;
        } else if (d < second_d) {
            second_d = d;
        }
    }

    if (best_i < 0) {
        return -1;
    }

    if (s_count > 1 && best_d > AMBIGUITY_RATIO * second_d) {
        ESP_LOGD(TAG, "ambiguous: %.2f vs %.2f", best_d, second_d);
        return -1;
    }

    *out_confidence = face_lbp_confidence(best_d);
    return best_i;
}

/* --------------------------------------------------------------------------
 * Enrolment
 * ------------------------------------------------------------------------ */
/** Caller holds s_lock. Commits the staged samples as a new subject. */
static void enrol_commit(void)
{
    if (s_count >= FACE_MAX_SUBJECTS) {
        ESP_LOGW(TAG, "no room for another face");
        return;
    }

    face_subject_t *sub = &s_subjects[s_count];
    memset(sub, 0, sizeof(*sub));
    sub->id      = s_next_id++;
    sub->samples = s_enrol_got;
    sub->added_epoch = (uint32_t)time(NULL);
    strlcpy(sub->name, s_enrol_name, sizeof(sub->name));

    memcpy(template_at(s_count, 0), s_enrol_buf,
           (size_t)s_enrol_got * FACE_DESC_LEN);

    s_count++;
    db_save();

    ESP_LOGI(TAG, "enrolled '%s' (id %u) from %u shots",
             sub->name, (unsigned)sub->id, (unsigned)s_enrol_got);
}

/* --------------------------------------------------------------------------
 * Per-frame work
 * ------------------------------------------------------------------------ */
static void publish_status(void)
{
    face_status_t copy;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    copy = s_status;
    xSemaphoreGive(s_lock);

    app_event_post(APP_EVT_FACE, &copy, sizeof(copy));
}

static jpg_scale_t scale_for(int w, int h)
{
    if (w <= ANALYSIS_MAX_W && h <= ANALYSIS_MAX_H)         return JPG_SCALE_NONE;
    if (w / 2 <= ANALYSIS_MAX_W && h / 2 <= ANALYSIS_MAX_H) return JPG_SCALE_2X;
    if (w / 4 <= ANALYSIS_MAX_W && h / 4 <= ANALYSIS_MAX_H) return JPG_SCALE_4X;
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

static void process_frame(void)
{
    const framesize_t fs   = bsp_camera_get_framesize();
    const int         full_w = resolution[fs].width;
    const int         full_h = resolution[fs].height;

    const jpg_scale_t scale = scale_for(full_w, full_h);
    const int         div   = scale_divisor(scale);
    const int         w     = full_w / div;
    const int         h     = full_h / div;

    if (w <= 0 || h <= 0 || (size_t)w * h * 2 > ANALYSIS_BYTES) {
        return;
    }
    if (!jpg2rgb565(s_jpeg, s_jpeg_len, s_rgb, scale)) {
        return;
    }

    uint16_t fx, fy, fw, fh;
    const bool found = face_locate(s_rgb, w, h, &fx, &fy, &fw, &fh);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.frame_w   = (uint16_t)w;
    s_status.frame_h   = (uint16_t)h;
    s_status.face_found = found;

    if (!found) {
        xSemaphoreGive(s_lock);
        publish_status();
        return;
    }

    s_status.x = fx;
    s_status.y = fy;
    s_status.w = fw;
    s_status.h = fh;
    xSemaphoreGive(s_lock);

    face_crop_gray(s_rgb, w, h, fx, fy, fw, fh, s_gray);
    face_lbp_equalize(s_gray, FACE_NORM_W, FACE_NORM_H);
    face_lbp_descriptor(s_gray, s_desc);

    const int64_t now = esp_timer_get_time();

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    if (s_enrolling) {
        if (now > s_enrol_until_us) {
            ESP_LOGW(TAG, "enrolment timed out with %u shot(s)",
                     (unsigned)s_enrol_got);
            s_enrolling = false;
        } else if (now - s_enrol_last_us >= (int64_t)ENROL_PERIOD_MS * 1000) {
            memcpy(s_enrol_buf + (size_t)s_enrol_got * FACE_DESC_LEN,
                   s_desc, FACE_DESC_LEN);
            s_enrol_got++;
            s_enrol_last_us = now;

            if (s_enrol_got >= FACE_SAMPLES_PER) {
                enrol_commit();
                s_enrolling = false;
            }
        }
        xSemaphoreGive(s_lock);
        publish_status();
        return;
    }

    uint8_t   confidence = 0;
    const int idx        = match(s_desc, &confidence);

    if (idx >= 0) {
        s_status.match_id    = s_subjects[idx].id;
        s_status.confidence  = confidence;
        strlcpy(s_status.match_name, s_subjects[idx].name,
                sizeof(s_status.match_name));
    }

    const bool submit = idx >= 0 &&
                        (s_last_submit_us == 0 ||
                         now - s_last_submit_us >= (int64_t)SUBMIT_COOLDOWN_MS * 1000);
    const uint16_t submit_id = idx >= 0 ? s_subjects[idx].id : 0;

    if (submit) {
        s_last_submit_us = now;
    }
    xSemaphoreGive(s_lock);

    /* Outside the lock: svc_access takes its own, writes the card and may
     * drive the strike. */
    if (submit) {
        svc_access_submit_face(submit_id, confidence);
    }
    publish_status();
}

static void worker_task(void *arg)
{
    (void)arg;

    while (1) {
        if (xSemaphoreTake(s_work, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        process_frame();

        /* Released only now, so the pump skips frames while we were busy
         * instead of queueing them up behind us. */
        s_jpeg_busy = false;
    }
}

/** Runs on the pump task: copy and hand off, nothing more. */
static void frame_cb(const uint8_t *jpeg, size_t len, void *ctx)
{
    (void)ctx;

    if (s_jpeg_busy || len > JPEG_HOLD_BYTES) {
        return;
    }

    const int64_t now    = esp_timer_get_time();
    const int64_t period = s_enrolling ? ENROL_PERIOD_MS : RECOGNISE_PERIOD_MS;
    if (s_last_run_us && now - s_last_run_us < period * 1000) {
        return;
    }
    s_last_run_us = now;

    memcpy(s_jpeg, jpeg, len);
    s_jpeg_len  = len;
    s_jpeg_busy = true;
    xSemaphoreGive(s_work);
}

/** Frames are wanted while recognition is on, and always while enrolling. */
static void sync_subscription(void)
{
    if (s_enabled || s_enrolling) {
        svc_media_add_frame_cb(frame_cb, NULL);
    } else {
        svc_media_remove_frame_cb(frame_cb);
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */
esp_err_t svc_face_set_enabled(bool enabled)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");

    s_enabled = enabled;
    sync_subscription();

    ESP_LOGI(TAG, "recognition %s", enabled ? "on" : "off");
    return ESP_OK;
}

bool svc_face_is_enabled(void)
{
    return s_enabled;
}

bool svc_face_ready(void)
{
    return s_ready;
}

void svc_face_status(face_status_t *out)
{
    if (!out) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_status;
    xSemaphoreGive(s_lock);
}

esp_err_t svc_face_enroll_begin(const char *name)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    ESP_RETURN_ON_FALSE(name && name[0], ESP_ERR_INVALID_ARG, TAG, "name");

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_count >= FACE_MAX_SUBJECTS) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    strlcpy(s_enrol_name, name, sizeof(s_enrol_name));
    s_enrol_got      = 0;
    s_enrol_last_us  = 0;
    s_enrol_until_us = esp_timer_get_time() + (int64_t)ENROL_TIMEOUT_S * 1000000;
    s_enrolling      = true;
    xSemaphoreGive(s_lock);

    sync_subscription();
    ESP_LOGI(TAG, "enrolling '%s'", name);
    return ESP_OK;
}

void svc_face_enroll_cancel(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_enrolling = false;
        s_enrol_got = 0;
        xSemaphoreGive(s_lock);
    }
    sync_subscription();
}

bool svc_face_enrolling(void)
{
    return s_enrolling;
}

uint8_t svc_face_enroll_progress(void)
{
    return s_enrol_got;
}

size_t svc_face_subject_count(void)
{
    return s_count;
}

esp_err_t svc_face_subject_get(size_t index, face_subject_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out");

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (index < s_count) {
        *out = s_subjects[index];
        err  = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t svc_face_subject_remove(size_t index)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (index < s_count) {
        const size_t tail = s_count - index - 1;
        if (tail) {
            memmove(&s_subjects[index], &s_subjects[index + 1],
                    tail * sizeof(s_subjects[0]));
            memmove(template_at((int)index, 0), template_at((int)index + 1, 0),
                    tail * FACE_SAMPLES_PER * FACE_DESC_LEN);
        }
        s_count--;
        memset(&s_subjects[s_count], 0, sizeof(s_subjects[0]));
        err = db_save();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t svc_face_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    s_work = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_lock && s_work, ESP_ERR_NO_MEM, TAG, "sync");

    s_jpeg      = heap_caps_malloc(JPEG_HOLD_BYTES, MALLOC_CAP_SPIRAM);
    s_rgb       = heap_caps_malloc(ANALYSIS_BYTES, MALLOC_CAP_SPIRAM);
    s_templates = heap_caps_calloc((size_t)FACE_MAX_SUBJECTS * FACE_SAMPLES_PER,
                                   FACE_DESC_LEN, MALLOC_CAP_SPIRAM);
    s_enrol_buf = heap_caps_calloc(FACE_SAMPLES_PER, FACE_DESC_LEN,
                                   MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_jpeg && s_rgb && s_templates && s_enrol_buf,
                        ESP_ERR_NO_MEM, TAG, "buffers");

    face_lbp_init();
    ESP_RETURN_ON_ERROR(face_locate_init(), TAG, "locator");

    db_load();

    /* Core 1, beside the frame pump: this is decode-bound work and core 0 is
     * where LVGL and the event loop live. */
    /* 12 KB: the descriptor histogram is ~1.9 KB of locals and the JPEG
     * decoder takes its own slice on top. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(worker_task, "face", 12288, NULL,
                                                3, NULL, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");

    s_ready = true;
    svc_access_face_set_available(true);

    /* Honour the stored policy, but never switch the door on for a face when
     * nobody is enrolled yet. */
    if (app_settings()->access_face_enabled && s_count > 0) {
        svc_face_set_enabled(true);
    }

    ESP_LOGI(TAG, "ready (LBPH, %u enrolled, no model file)", (unsigned)s_count);
    return ESP_OK;
}
