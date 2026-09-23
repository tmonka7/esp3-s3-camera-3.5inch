#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_events.h"
#include "app_settings.h"
#include "app_time.h"
#include "bsp_board.h"
#include "bsp_rfid.h"
#include "bsp_storage.h"
#include "svc_access.h"
#include "svc_media.h"
#include "svc_notify.h"

static const char *TAG = "access";

#define NVS_NAMESPACE   "access"
#define NVS_KEY_CREDS   "creds"
#define STORE_VERSION   1

/* How long the same card is ignored after a read, so that leaving a card on
 * the reader does not re-trigger the door every poll. */
#define SAME_CARD_GAP_MS    2500
#define POLL_PERIOD_MS      150

typedef struct {
    uint32_t      version;
    uint16_t      count;
    access_cred_t creds[ACCESS_MAX_CREDENTIALS];
} cred_store_t;

static cred_store_t      s_store;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_relock_timer;

static access_lock_state_t s_lock_state = ACCESS_LOCK_LOCKED;
static int64_t             s_relock_at_us;

static bool     s_enrolling;
static int64_t  s_enroll_until_us;

static uint8_t  s_fail_streak;
static int64_t  s_lockout_until_us;

static bool     s_face_available;

static access_event_t s_history[ACCESS_HISTORY_DEPTH];
static size_t         s_history_count;

/* --------------------------------------------------------------------------
 * Persistence
 * ------------------------------------------------------------------------ */
static esp_err_t store_load(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no credential store yet");
        s_store.version = STORE_VERSION;
        s_store.count   = 0;
        return ESP_OK;
    }

    size_t len = sizeof(s_store);
    err = nvs_get_blob(h, NVS_KEY_CREDS, &s_store, &len);
    nvs_close(h);

    /* Credentials live in their own namespace rather than in the settings
     * blob on purpose: a settings version bump must never take the door keys
     * with it. The same reasoning applies here, so a layout change resets
     * only the credentials. */
    if (err != ESP_OK || len != sizeof(s_store) || s_store.version != STORE_VERSION) {
        ESP_LOGW(TAG, "credential store unreadable -- starting empty");
        memset(&s_store, 0, sizeof(s_store));
        s_store.version = STORE_VERSION;
        return ESP_OK;
    }

    if (s_store.count > ACCESS_MAX_CREDENTIALS) {
        s_store.count = ACCESS_MAX_CREDENTIALS;
    }
    ESP_LOGI(TAG, "%u credentials loaded", (unsigned)s_store.count);
    return ESP_OK;
}

/** Caller holds s_lock. */
static esp_err_t store_save(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open");

    esp_err_t err = nvs_set_blob(h, NVS_KEY_CREDS, &s_store, sizeof(s_store));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* --------------------------------------------------------------------------
 * Strike control
 *
 * Every path that moves the bolt goes through set_lock(), so there is exactly
 * one place that touches the GPIO and exactly one place that posts the state.
 * ------------------------------------------------------------------------ */
static void drive_strike(bool unlocked)
{
    if (BSP_LOCK_PIN < 0) {
        return;
    }
    const int level = unlocked ? BSP_LOCK_ACTIVE_HIGH : !BSP_LOCK_ACTIVE_HIGH;
    gpio_set_level((gpio_num_t)BSP_LOCK_PIN, level);
}

static void publish_lock_state(void)
{
    const access_lock_state_t st = s_lock_state;
    app_event_post(APP_EVT_LOCK_STATE, &st, sizeof(st));
}

static void relock_cb(void *arg)
{
    (void)arg;
    drive_strike(false);
    s_lock_state   = ACCESS_LOCK_LOCKED;
    s_relock_at_us = 0;
    ESP_LOGI(TAG, "relocked");
    publish_lock_state();
}

static void unlock_for(uint32_t ms)
{
    if (BSP_LOCK_PIN < 0) {
        ESP_LOGW(TAG, "granted, but no strike GPIO is configured");
        return;
    }

    drive_strike(true);
    s_lock_state   = ACCESS_LOCK_UNLOCKED;
    s_relock_at_us = esp_timer_get_time() + (int64_t)ms * 1000;

    /* Restarting rather than stacking: a second grant while the door is
     * already open extends the window instead of relocking early. */
    esp_timer_stop(s_relock_timer);
    esp_timer_start_once(s_relock_timer, (uint64_t)ms * 1000);

    ESP_LOGI(TAG, "unlocked for %u ms", (unsigned)ms);
    publish_lock_state();
}

/* --------------------------------------------------------------------------
 * Audit trail
 * ------------------------------------------------------------------------ */
static void append_access_log(const access_event_t *e)
{
    if (!bsp_storage_mounted()) {
        return;
    }

    char date[16];
    app_time_format(date, sizeof(date), "%Y-%m-%d");

    char path[96];
    snprintf(path, sizeof(path), BSP_SD_MOUNT_POINT "/access/%s.csv", date);

    const bool fresh = (access(path, F_OK) != 0);

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGW(TAG, "cannot append to %s", path);
        return;
    }
    if (fresh) {
        fprintf(f, "time,result,method,name,credential,confidence,snapshot\n");
    }
    fprintf(f, "%s,%s,%s,%s,%s,%u,%s\n",
            e->stamp,
            svc_access_result_name(e->result),
            e->kind == ACCESS_CRED_FACE ? "face" : "card",
            e->name[0] ? e->name : "-",
            e->credential,
            (unsigned)e->confidence,
            e->snapshot[0] ? e->snapshot : "-");
    fclose(f);
}

/** Caller must NOT hold s_lock: this writes to the card and takes a photo. */
static void record_event(access_event_t *e)
{
    app_time_format(e->stamp, sizeof(e->stamp), "%H:%M:%S");

    const app_settings_t *cfg = app_settings();

    if (cfg->access_beep &&
        (e->result == ACCESS_GRANTED || e->result == ACCESS_GRANTED_MANUAL ||
         e->result == ACCESS_ENROLLED)) {
        svc_notify_beep();
    }

    if (cfg->access_snapshot) {
        /* A fresh grab, not the detector's last frame: the useful photo is
         * of whoever is standing there now. */
        if (svc_media_snapshot(e->snapshot, sizeof(e->snapshot)) != ESP_OK) {
            e->snapshot[0] = '\0';
        }
    }

    append_access_log(e);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_history_count < ACCESS_HISTORY_DEPTH) {
            s_history_count++;
        }
        memmove(&s_history[1], &s_history[0],
                (s_history_count - 1) * sizeof(s_history[0]));
        s_history[0] = *e;
        xSemaphoreGive(s_lock);
    }

    app_event_post(APP_EVT_ACCESS, e, sizeof(*e));
}

/* --------------------------------------------------------------------------
 * Decision
 * ------------------------------------------------------------------------ */
static bool in_lockout(void)
{
    return s_lockout_until_us != 0 && esp_timer_get_time() < s_lockout_until_us;
}

/** Caller holds s_lock. Returns the index or -1. */
static int find_card(const bsp_rfid_uid_t *uid)
{
    for (uint16_t i = 0; i < s_store.count; i++) {
        const access_cred_t *c = &s_store.creds[i];
        if (c->kind != ACCESS_CRED_CARD || c->uid_len != uid->len) {
            continue;
        }
        if (memcmp(c->uid, uid->bytes, uid->len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/** Caller holds s_lock. Returns the index or -1. */
static int find_face(uint16_t face_id)
{
    for (uint16_t i = 0; i < s_store.count; i++) {
        const access_cred_t *c = &s_store.creds[i];
        if (c->kind == ACCESS_CRED_FACE && c->face_id == face_id) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Caller holds s_lock and has already checked there is room.
 *
 * `name` NULL auto-names it "Card N". Returns the new record so the caller
 * can report what it ended up called.
 */
static const access_cred_t *store_append_card(const uint8_t *uid, uint8_t uid_len,
                                              const char *name)
{
    access_cred_t *c = &s_store.creds[s_store.count];
    memset(c, 0, sizeof(*c));

    c->kind    = ACCESS_CRED_CARD;
    c->uid_len = uid_len;
    memcpy(c->uid, uid, uid_len);

    if (name && name[0]) {
        strlcpy(c->name, name, sizeof(c->name));
    } else {
        snprintf(c->name, sizeof(c->name), "Card %u", (unsigned)(s_store.count + 1));
    }

    c->enabled     = true;
    c->added_epoch = (uint32_t)time(NULL);
    s_store.count++;
    return c;
}

/** Caller holds s_lock. Records a successful use against the credential. */
static void mark_used(int index)
{
    access_cred_t *c   = &s_store.creds[index];
    c->last_used_epoch = (uint32_t)time(NULL);
    c->use_count++;
    /* Deliberately not committed here: the counters are a convenience, and
     * writing NVS on every badge-in would wear the partition down. They are
     * flushed whenever the credential list is actually edited. */
}

/**
 * Caller holds s_lock, so this must not block: no beeping here, because
 * svc_notify_beep() sleeps for the length of the tone and would hold the
 * credential list hostage while it did. record_event() does the audible part
 * once the lock is back.
 */
static void apply_outcome(access_result_t result, uint32_t strike_ms)
{
    const app_settings_t *cfg = app_settings();

    if (result == ACCESS_GRANTED || result == ACCESS_GRANTED_MANUAL) {
        s_fail_streak      = 0;
        s_lockout_until_us = 0;
        unlock_for(strike_ms);
        return;
    }

    if (result == ACCESS_DENIED_UNKNOWN || result == ACCESS_DENIED_DISABLED) {
        if (s_fail_streak < 255) {
            s_fail_streak++;
        }
        if (cfg->access_max_failures && s_fail_streak >= cfg->access_max_failures) {
            s_lockout_until_us = esp_timer_get_time() +
                                 (int64_t)cfg->access_lockout_s * 1000000;
            ESP_LOGW(TAG, "%u failures -- reader ignored for %u s",
                     (unsigned)s_fail_streak, (unsigned)cfg->access_lockout_s);
        }
    }
}

/* --------------------------------------------------------------------------
 * Card path
 * ------------------------------------------------------------------------ */
static void handle_card(const bsp_rfid_uid_t *uid)
{
    access_event_t e = { .kind = ACCESS_CRED_CARD };
    bsp_rfid_uid_str(uid, e.credential, sizeof(e.credential));

    const app_settings_t *cfg = app_settings();
    uint32_t              strike_ms = cfg->access_strike_ms;
    bool                  save      = false;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }

    if (s_enrolling) {
        if (esp_timer_get_time() > s_enroll_until_us) {
            s_enrolling = false;
        }
    }

    if (s_enrolling) {
        const int existing = find_card(uid);
        if (existing >= 0) {
            /* Re-presenting a known card during enrolment is almost always a
             * user checking whether it took. Say so rather than duplicating. */
            e.result = ACCESS_ENROLLED;
            strlcpy(e.name, s_store.creds[existing].name, sizeof(e.name));
        } else if (s_store.count >= ACCESS_MAX_CREDENTIALS) {
            e.result = ACCESS_DENIED_UNKNOWN;
            ESP_LOGW(TAG, "credential list is full");
        } else {
            const access_cred_t *c = store_append_card(uid->bytes, uid->len, NULL);
            e.result = ACCESS_ENROLLED;
            strlcpy(e.name, c->name, sizeof(e.name));
            save = true;
        }
        s_enrolling = false;
    } else if (!cfg->access_card_enabled) {
        e.result = ACCESS_DENIED_DISABLED;
    } else if (in_lockout()) {
        e.result = ACCESS_DENIED_LOCKOUT;
    } else {
        const int idx = find_card(uid);
        if (idx < 0) {
            e.result = ACCESS_DENIED_UNKNOWN;
        } else if (!s_store.creds[idx].enabled) {
            e.result = ACCESS_DENIED_DISABLED;
            strlcpy(e.name, s_store.creds[idx].name, sizeof(e.name));
        } else {
            e.result = ACCESS_GRANTED;
            strlcpy(e.name, s_store.creds[idx].name, sizeof(e.name));
            mark_used(idx);
        }
    }

    if (save) {
        store_save();
    }
    apply_outcome(e.result, strike_ms);
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "card %s -> %s%s%s", e.credential,
             svc_access_result_name(e.result),
             e.name[0] ? " / " : "", e.name);

    record_event(&e);
}

/* --------------------------------------------------------------------------
 * Reader task
 * ------------------------------------------------------------------------ */
static void reader_task(void *arg)
{
    (void)arg;

    bsp_rfid_uid_t last      = { 0 };
    int64_t        last_us   = 0;

    while (1) {
        bsp_rfid_uid_t uid;
        const esp_err_t err = bsp_rfid_poll(&uid);

        if (err == ESP_OK) {
            const int64_t now = esp_timer_get_time();
            const bool    same_card_again =
                bsp_rfid_uid_equal(&uid, &last) &&
                (now - last_us) < (int64_t)SAME_CARD_GAP_MS * 1000;

            if (!same_card_again) {
                last    = uid;
                last_us = now;
                handle_card(&uid);
            } else {
                /* Refresh the timestamp so a card left sitting on the reader
                 * keeps being suppressed rather than re-firing every 2.5 s. */
                last_us = now;
            }
            bsp_rfid_halt();
        } else if (err != ESP_ERR_NOT_FOUND) {
            /* NOT_FOUND is an empty field, which is the normal state and not
             * worth a log line. Anything else is a real exchange failure. */
            ESP_LOGD(TAG, "poll: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

/* --------------------------------------------------------------------------
 * Face path
 * ------------------------------------------------------------------------ */
esp_err_t svc_access_submit_face(uint16_t face_id, uint8_t confidence)
{
    const app_settings_t *cfg = app_settings();

    if (!cfg->access_face_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (confidence < cfg->access_face_threshold) {
        ESP_LOGD(TAG, "face %u at %u%% is below the %u%% threshold",
                 (unsigned)face_id, (unsigned)confidence,
                 (unsigned)cfg->access_face_threshold);
        return ESP_ERR_NOT_FOUND;
    }

    access_event_t e = { .kind = ACCESS_CRED_FACE, .confidence = confidence };
    snprintf(e.credential, sizeof(e.credential), "face:%u", (unsigned)face_id);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (in_lockout()) {
        e.result = ACCESS_DENIED_LOCKOUT;
    } else {
        const int idx = find_face(face_id);
        if (idx < 0) {
            e.result = ACCESS_DENIED_UNKNOWN;
        } else if (!s_store.creds[idx].enabled) {
            e.result = ACCESS_DENIED_DISABLED;
            strlcpy(e.name, s_store.creds[idx].name, sizeof(e.name));
        } else {
            e.result = ACCESS_GRANTED;
            strlcpy(e.name, s_store.creds[idx].name, sizeof(e.name));
            mark_used(idx);
        }
    }

    apply_outcome(e.result, cfg->access_strike_ms);
    xSemaphoreGive(s_lock);

    record_event(&e);
    return e.result == ACCESS_GRANTED ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void svc_access_face_set_available(bool available)
{
    s_face_available = available;
}

bool svc_access_face_available(void)
{
    return s_face_available;
}

/* --------------------------------------------------------------------------
 * Public control
 * ------------------------------------------------------------------------ */
esp_err_t svc_access_unlock_manual(void)
{
    const app_settings_t *cfg = app_settings();

    access_event_t e = {
        .result = ACCESS_GRANTED_MANUAL,
        .kind   = ACCESS_CRED_CARD,
    };
    strlcpy(e.credential, "panel", sizeof(e.credential));
    strlcpy(e.name, "Manual", sizeof(e.name));

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    apply_outcome(ACCESS_GRANTED_MANUAL, cfg->access_strike_ms);
    xSemaphoreGive(s_lock);

    record_event(&e);
    return ESP_OK;
}

esp_err_t svc_access_lock_now(void)
{
    esp_timer_stop(s_relock_timer);
    relock_cb(NULL);
    return ESP_OK;
}

access_lock_state_t svc_access_lock_state(void)
{
    return s_lock_state;
}

uint32_t svc_access_relock_in(void)
{
    if (s_lock_state != ACCESS_LOCK_UNLOCKED || s_relock_at_us == 0) {
        return 0;
    }
    const int64_t left = s_relock_at_us - esp_timer_get_time();
    return left > 0 ? (uint32_t)((left + 999999) / 1000000) : 0;
}

esp_err_t svc_access_enroll_begin(uint32_t timeout_s)
{
    ESP_RETURN_ON_FALSE(bsp_rfid_present(), ESP_ERR_NOT_SUPPORTED, TAG, "no reader");

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_enrolling       = true;
    s_enroll_until_us = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "enrolment open for %u s", (unsigned)timeout_s);
    return ESP_OK;
}

void svc_access_enroll_cancel(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_enrolling = false;
        xSemaphoreGive(s_lock);
    }
}

bool svc_access_enrolling(void)
{
    if (!s_enrolling) {
        return false;
    }
    /* Checked lazily rather than on a timer: nothing needs to happen the
     * instant it expires, only that it is not still open when asked. */
    if (esp_timer_get_time() > s_enroll_until_us) {
        s_enrolling = false;
    }
    return s_enrolling;
}

/* --------------------------------------------------------------------------
 * Credential list
 * ------------------------------------------------------------------------ */
size_t svc_access_cred_count(void)
{
    return s_store.count;
}

esp_err_t svc_access_cred_get(size_t index, access_cred_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out");

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (index < s_store.count) {
        *out = s_store.creds[index];
        err  = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t svc_access_cred_add_card(const uint8_t *uid, uint8_t uid_len,
                                   const char *name)
{
    ESP_RETURN_ON_FALSE(uid, ESP_ERR_INVALID_ARG, TAG, "uid");
    ESP_RETURN_ON_FALSE(uid_len == 4 || uid_len == 7 || uid_len == 10,
                        ESP_ERR_INVALID_SIZE, TAG, "uid length %u",
                        (unsigned)uid_len);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err;
    bsp_rfid_uid_t probe = { .len = uid_len };
    memcpy(probe.bytes, uid, uid_len);

    if (find_card(&probe) >= 0) {
        /* Two entries for one card would mean the second could never be
         * reached, and disabling the visible one would not lock the card out. */
        err = ESP_ERR_INVALID_STATE;
    } else if (s_store.count >= ACCESS_MAX_CREDENTIALS) {
        err = ESP_ERR_NO_MEM;
    } else {
        store_append_card(uid, uid_len, name);
        err = store_save();
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t svc_access_cred_rename(size_t index, const char *name)
{
    ESP_RETURN_ON_FALSE(name, ESP_ERR_INVALID_ARG, TAG, "name");

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (index < s_store.count) {
        strlcpy(s_store.creds[index].name, name, ACCESS_NAME_LEN);
        err = store_save();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t svc_access_cred_set_enabled(size_t index, bool enabled)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (index < s_store.count) {
        s_store.creds[index].enabled = enabled;
        err = store_save();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t svc_access_cred_remove(size_t index)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (index < s_store.count) {
        const size_t tail = s_store.count - index - 1;
        if (tail) {
            memmove(&s_store.creds[index], &s_store.creds[index + 1],
                    tail * sizeof(s_store.creds[0]));
        }
        s_store.count--;
        memset(&s_store.creds[s_store.count], 0, sizeof(s_store.creds[0]));
        err = store_save();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t svc_access_cred_remove_all(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memset(s_store.creds, 0, sizeof(s_store.creds));
    s_store.count = 0;
    const esp_err_t err = store_save();
    xSemaphoreGive(s_lock);

    ESP_LOGW(TAG, "all credentials erased");
    return err;
}

size_t svc_access_history(access_event_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return 0;
    }
    const size_t n = s_history_count < max ? s_history_count : max;
    memcpy(out, s_history, n * sizeof(out[0]));
    xSemaphoreGive(s_lock);
    return n;
}

const char *svc_access_result_name(access_result_t r)
{
    switch (r) {
    case ACCESS_GRANTED:          return "granted";
    case ACCESS_DENIED_UNKNOWN:   return "unknown";
    case ACCESS_DENIED_DISABLED:  return "disabled";
    case ACCESS_DENIED_LOCKOUT:   return "lockout";
    case ACCESS_GRANTED_MANUAL:   return "manual";
    case ACCESS_ENROLLED:         return "enrolled";
    }
    return "?";
}

/* --------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------ */
esp_err_t svc_access_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    /* The strike is driven to locked before anything else can grant, and
     * before the reader is even awake.
     *
     * This closes the software window only. The pin still floats from reset
     * until this line runs, so the relay input needs a pull-down (or a
     * pull-up for an active-low module) to stay locked across a reboot --
     * see BSP_LOCK_PIN in menuconfig. */
    if (BSP_LOCK_PIN >= 0) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << (BSP_LOCK_PIN & 0x3F),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "strike gpio");
        drive_strike(false);
    } else {
        ESP_LOGW(TAG, "no strike GPIO configured -- decisions are logged only");
    }

    const esp_timer_create_args_t targs = {
        .callback = relock_cb,
        .name     = "relock",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_relock_timer), TAG, "timer");

    ESP_RETURN_ON_ERROR(store_load(), TAG, "store");

    /* /sdcard/access is created by bsp_storage at mount time, so it exists
     * even when the card is inserted after boot. */

    const esp_err_t rerr = bsp_rfid_init();
    if (rerr == ESP_OK) {
        ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(reader_task, "rfid", 4096, NULL,
                                                    4, NULL, 0) == pdPASS,
                            ESP_ERR_NO_MEM, TAG, "reader task");
    } else {
        /* A missing reader is not fatal: the panel still unlocks manually and
         * the face path, when it exists, is unaffected. */
        ESP_LOGW(TAG, "reader unavailable (%s) -- card entry is off",
                 esp_err_to_name(rerr));
    }

    ESP_LOGI(TAG, "access control ready (strike GPIO %d, %s)",
             BSP_LOCK_PIN, BSP_LOCK_ACTIVE_HIGH ? "active high" : "active low");
    return ESP_OK;
}
