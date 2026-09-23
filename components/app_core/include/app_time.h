/* Wall clock.
 *
 * The panel is a manual-clock device: it restores the time from the on-board
 * PCF85063 at boot and is otherwise set by hand from Settings -> Date & Time.
 * There is no SNTP, so the unit is fully usable on an isolated network or
 * with no network at all.
 *
 * The RTC holds UTC. Everything the user sees and types is local time, and
 * the configured POSIX timezone is what converts between them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Restores the clock from the RTC and applies the configured timezone. */
esp_err_t app_time_init(void);

/** True once the RTC or a manual entry has given us a plausible wall clock. */
bool app_time_is_valid(void);

/** True when a PCF85063 answered, i.e. the time survives a power cut. */
bool app_time_has_rtc(void);

/**
 * Sets the clock from local wall-clock time -- what the user typed on the
 * Date & Time page. `tm_isdst` is ignored; the timezone decides.
 */
esp_err_t app_time_set_local(const struct tm *local_tm);

/** Sets the clock from UTC. */
esp_err_t app_time_set_utc(const struct tm *utc_tm);

/** Current local time, for pre-filling the editor. */
void app_time_get_local(struct tm *out);

/** Re-reads the TZ string from settings and applies it. */
void app_time_apply_timezone(void);

/** strftime against local time, e.g. "%Y-%m-%d %H:%M". */
void app_time_format(char *out, size_t len, const char *fmt);

/** Filename-safe stamp, e.g. "2026-09-22_14-35-07". */
void app_time_stamp_filename(char *out, size_t len);

/** Days in `month` (1..12) of `year`, honouring leap years. */
int app_time_days_in_month(int year, int month);

#ifdef __cplusplus
}
#endif
