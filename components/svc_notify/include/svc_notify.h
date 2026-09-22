/* Alerts.
 *
 * Deliberately generic: it takes a title, a detail line and an optional
 * file, so the detector does not have to know whether an alert ends up as a
 * beep, a line in the event log or an HTTP POST. All three happen off the
 * caller's task -- a detection must never be delayed by a network timeout.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t svc_notify_init(void);

/**
 * Queues an alert. Returns immediately; delivery happens on the notify task.
 *
 * @param title      short label, e.g. "Person detected"
 * @param detail     free text, e.g. "Front Door 10:24:36"
 * @param file_path  optional file to reference in the webhook, or NULL
 */
esp_err_t svc_notify_alert(const char *title, const char *detail, const char *file_path);

/** Single short beep, for UI feedback. No-op when no buzzer is fitted. */
void svc_notify_beep(void);

#ifdef __cplusplus
}
#endif
