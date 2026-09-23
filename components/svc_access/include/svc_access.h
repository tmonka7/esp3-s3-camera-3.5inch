/* Door access control: credentials, the unlock decision and the strike.
 *
 * The readers are dumb -- they produce an identifier. Everything that decides
 * what an identifier is worth lives here, so adding a second reader type
 * later means writing a driver, not touching policy.
 *
 * Configured policy is "card or face unlocks", i.e. either credential alone
 * opens the door. See the README for what that means in practice; the short
 * version is that a card UID can be cloned and a face can be presented as a
 * photograph, so this is a convenience lock, not a secure one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "bsp_rfid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACCESS_MAX_CREDENTIALS  32
#define ACCESS_NAME_LEN         24
#define ACCESS_CRED_STR_LEN     24
#define ACCESS_HISTORY_DEPTH    20

typedef enum {
    ACCESS_CRED_CARD = 0,
    ACCESS_CRED_FACE,
} access_cred_kind_t;

typedef struct {
    access_cred_kind_t kind;
    uint8_t            uid[BSP_RFID_UID_MAX];   /* ACCESS_CRED_CARD */
    uint8_t            uid_len;
    uint16_t           face_id;                 /* ACCESS_CRED_FACE */
    char               name[ACCESS_NAME_LEN];
    bool               enabled;
    uint32_t           added_epoch;
    uint32_t           last_used_epoch;
    uint32_t           use_count;
} access_cred_t;

typedef enum {
    ACCESS_GRANTED = 0,
    ACCESS_DENIED_UNKNOWN,      /* no matching credential            */
    ACCESS_DENIED_DISABLED,     /* known, but switched off           */
    ACCESS_DENIED_LOCKOUT,      /* too many failures, reader ignored */
    ACCESS_GRANTED_MANUAL,      /* unlocked from the panel           */
    ACCESS_ENROLLED,            /* card captured in enrolment mode   */
} access_result_t;

typedef struct {
    access_result_t    result;
    access_cred_kind_t kind;
    char               name[ACCESS_NAME_LEN];       /* empty when unknown  */
    char               credential[ACCESS_CRED_STR_LEN]; /* UID hex or face */
    uint8_t            confidence;                  /* face only, 0..100   */
    char               stamp[16];                   /* "14:02:37"          */
    char               snapshot[72];                /* empty if not saved  */
} access_event_t;

typedef enum {
    ACCESS_LOCK_LOCKED = 0,
    ACCESS_LOCK_UNLOCKED,
} access_lock_state_t;

/**
 * Configures the strike output (locked), brings up the reader and starts the
 * polling task. Call after bsp_display_init(), which owns the SPI bus.
 */
esp_err_t svc_access_init(void);

/* ---- lock ------------------------------------------------------------- */

/** Energises the strike and schedules the automatic relock. */
esp_err_t svc_access_unlock_manual(void);

/** Relocks immediately, cancelling any pending auto-relock. */
esp_err_t svc_access_lock_now(void);

access_lock_state_t svc_access_lock_state(void);

/** Seconds remaining before the automatic relock, 0 when locked. */
uint32_t svc_access_relock_in(void);

/* ---- enrolment -------------------------------------------------------- *
 *
 * In enrolment mode the next card read is stored as a new credential instead
 * of being judged against the existing ones. It times out on its own so the
 * panel cannot be left in a state where any card presented gets added.
 */
esp_err_t svc_access_enroll_begin(uint32_t timeout_s);
void      svc_access_enroll_cancel(void);
bool      svc_access_enrolling(void);

/* ---- credentials ------------------------------------------------------ */

size_t    svc_access_cred_count(void);
esp_err_t svc_access_cred_get(size_t index, access_cred_t *out);
esp_err_t svc_access_cred_rename(size_t index, const char *name);
esp_err_t svc_access_cred_set_enabled(size_t index, bool enabled);
esp_err_t svc_access_cred_remove(size_t index);
esp_err_t svc_access_cred_remove_all(void);

/* ---- face ------------------------------------------------------------- *
 *
 * The recognition backend calls this when it matches an enrolled subject.
 * Kept separate from the card path so the two can be reasoned about (and
 * disabled) independently.
 */
esp_err_t svc_access_submit_face(uint16_t face_id, uint8_t confidence);

/**
 * A recogniser calls this once it is up, so the UI can show the face controls
 * instead of guessing whether anything is behind them.
 */
void svc_access_face_set_available(bool available);
bool svc_access_face_available(void);

/* ---- history ---------------------------------------------------------- */

/** Most recent decisions, newest first. Returns how many were written. */
size_t svc_access_history(access_event_t *out, size_t max);

const char *svc_access_result_name(access_result_t r);

#ifdef __cplusplus
}
#endif
