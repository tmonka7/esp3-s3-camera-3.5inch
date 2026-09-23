#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Restores the system clock from the on-board PCF85063 if available,
 * applies the configured timezone, and leaves time sync as a manual task
 * so the unit works fully offline.
 */
esp_err_t app_time_init(void);

/** True once the RTC or SNTP has given us a plausible wall clock. */
bool app_time_is_valid(void);

/** Sets both the system clock and the RTC. */
esp_err_t app_time_set(const struct tm *tm_utc);

/** Re-reads the TZ string from settings and applies it. */
void app_time_apply_timezone(void);

/** "2026-09-22 14:35:07" style stamps for the UI and for file names. */
void app_time_format(char *out, size_t len, const char *fmt);

/** Filename-safe stamp, e.g. "2026-09-22_14-35-07". */
void app_time_stamp_filename(char *out, size_t len);

#ifdef __cplusplus
}
#endif
