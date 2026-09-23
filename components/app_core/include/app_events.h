/* The one event bus every service publishes on and the UI subscribes to.
 *
 * Services never touch LVGL objects. They post an event with a small
 * by-value payload; the UI handler runs on the event-loop task, takes the
 * LVGL lock and updates widgets. That keeps every driver free of UI code and
 * keeps LVGL single-threaded.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(APP_EVENT);

typedef enum {
    APP_EVT_BOOT_PROGRESS = 0,   /* app_boot_progress_t */
    APP_EVT_WIFI_STATE,          /* app_wifi_state_t    */
    APP_EVT_TIME_SYNC,           /* no payload          */
    APP_EVT_SETTINGS_CHANGED,    /* app_settings_key_t  */

    APP_EVT_CAMERA_STATE,        /* app_camera_state_t  */
    APP_EVT_CAMERA_FRAME,        /* no payload -- pull the latest frame */
    APP_EVT_DETECTION,           /* detect_event_t      */
    APP_EVT_RECORD_STATE,        /* media_record_state_t */

    APP_EVT_UART_RX,             /* app_uart_line_t     */
    APP_EVT_UART_STATE,          /* app_link_state_t    */

    APP_EVT_MODBUS_STATE,        /* app_link_state_t    */
    APP_EVT_MODBUS_RESULT,       /* modbus_result_t     */

    APP_EVT_POWER_UPDATE,        /* power_status_t      */
    APP_EVT_SWITCH_UPDATE,       /* switch_update_t     */
    APP_EVT_TEMP_UPDATE,         /* temp_status_t       */
    APP_EVT_STORAGE_UPDATE,      /* bsp_storage_info_t  */

    APP_EVT_ACCESS,              /* access_event_t      */
    APP_EVT_LOCK_STATE,          /* access_lock_state_t */
    APP_EVT_FACE,                /* face_status_t       */
} app_event_id_t;

typedef struct {
    int  percent;
    char message[48];
} app_boot_progress_t;

typedef enum {
    APP_LINK_DOWN = 0,
    APP_LINK_CONNECTING,
    APP_LINK_UP,
    APP_LINK_ERROR,
} app_link_state_t;

typedef struct {
    app_link_state_t state;
    char             ip[16];
    char             ssid[33];
    int              rssi;
} app_wifi_state_t;

typedef enum {
    APP_CAMERA_OFF = 0,
    APP_CAMERA_STARTING,
    APP_CAMERA_LIVE,
    APP_CAMERA_ERROR,
} app_camera_state_t;

/** Starts the loop. Must run before any service or UI code. */
esp_err_t app_events_init(void);

/** Posts an event. `data` is copied, so stack payloads are fine. */
esp_err_t app_event_post(app_event_id_t id, const void *data, size_t size);

/** Posts from an ISR. */
esp_err_t app_event_post_isr(app_event_id_t id, const void *data, size_t size);

/** Subscribes to one event id, or to ESP_EVENT_ANY_ID. */
esp_err_t app_event_subscribe(app_event_id_t id, esp_event_handler_t handler, void *arg);
esp_err_t app_event_subscribe_any(esp_event_handler_t handler, void *arg);
esp_err_t app_event_unsubscribe(app_event_id_t id, esp_event_handler_t handler);

#ifdef __cplusplus
}
#endif
