/* Persisted configuration.
 *
 * Everything the Settings screen can change lives in one struct that is
 * written to NVS as a single blob. A blob keeps the whole configuration
 * atomically consistent and makes "factory reset" a one-line erase.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SWITCH_COUNT        10
#define APP_SWITCH_NAME_LEN     20
#define APP_CAMERA_COUNT        4
#define APP_CAMERA_NAME_LEN     20

/* Bump whenever the struct layout changes; a mismatch falls back to
 * defaults instead of reading a stale layout. */
#define APP_SETTINGS_VERSION    3

typedef enum {
    APP_LANG_EN = 0,
    APP_LANG_KO,
    APP_LANG_ZH,
    APP_LANG_MAX,
} app_language_t;

/** Where a room switch is physically wired. */
typedef enum {
    SWITCH_BIND_NONE = 0,   /* UI-only, state kept in NVS       */
    SWITCH_BIND_GPIO,       /* direct ESP32 GPIO                */
    SWITCH_BIND_IOEXP,      /* TCA9554 bit                      */
    SWITCH_BIND_MODBUS,     /* Modbus coil on the RTU/TCP master */
} switch_bind_t;

typedef struct {
    char          name[APP_SWITCH_NAME_LEN];
    switch_bind_t bind;
    int16_t       gpio;         /* SWITCH_BIND_GPIO / _IOEXP bit  */
    uint8_t       slave_addr;   /* SWITCH_BIND_MODBUS             */
    uint16_t      coil_addr;
    bool          state;        /* last commanded state, restored at boot */
    bool          invert;
} app_switch_cfg_t;

/** Where a measurement comes from. Used by both temperature and power. */
typedef enum {
    SRC_NONE = 0,
    SRC_I2C_SHT3X,
    SRC_MODBUS,
    SRC_MANUAL,
} app_source_t;

typedef struct {
    uint32_t version;

    /* ---- system ---- */
    app_language_t language;
    uint8_t        brightness;        /* 0..100                        */
    uint16_t       auto_sleep_min;    /* 0 = never                     */
    char           timezone[40];      /* POSIX TZ, e.g. "KST-9"        */
    char           ntp_server[48];   /* unused: the clock is set by hand */
    char           pin_code[9];       /* empty = screen lock disabled  */

    /* ---- network ---- */
    bool     wifi_enabled;
    char     wifi_ssid[33];
    char     wifi_pass[65];

    /* ---- camera ---- */
    uint8_t  cam_framesize;           /* framesize_t                   */
    uint8_t  cam_quality;             /* 10..63                        */
    bool     cam_hmirror;
    bool     cam_vflip;
    uint8_t  cam_active;              /* index into cam_names          */
    char     cam_names[APP_CAMERA_COUNT][APP_CAMERA_NAME_LEN];

    /* ---- detection ---- */
    bool     detect_enabled;
    bool     detect_notify;
    uint8_t  detect_sensitivity;      /* 1..10                         */
    uint16_t detect_cooldown_s;       /* min gap between notifications */
    bool     detect_record_clip;      /* auto-record on a detection    */
    char     notify_webhook[128];     /* empty = local alert only      */

    /* ---- general-purpose UART page ---- */
    uint32_t uart_baud;
    uint8_t  uart_databits;           /* 5..8                          */
    uint8_t  uart_parity;             /* 0 none, 1 odd, 2 even         */
    uint8_t  uart_stopbits;           /* 1 or 2                        */
    bool     uart_hex_view;
    bool     uart_log_to_sd;

    /* ---- Modbus ---- */
    bool     mb_rtu_enabled;
    uint32_t mb_rtu_baud;
    uint8_t  mb_rtu_parity;
    bool     mb_tcp_enabled;
    char     mb_tcp_host[32];
    uint16_t mb_tcp_port;
    uint16_t mb_poll_ms;

    /* ---- power system (read through the Modbus master) ---- */
    app_source_t power_source;
    uint8_t      power_slave;
    uint16_t     power_reg_voltage;
    uint16_t     power_reg_current;
    uint16_t     power_reg_power;
    uint16_t     power_reg_energy;
    uint16_t     power_coil_main;
    uint16_t     power_coil_solar;
    uint16_t     power_coil_battery;
    uint16_t     power_coil_ups;

    /* ---- temperature ---- */
    app_source_t temp_source;
    uint8_t      temp_slave;
    uint16_t     temp_reg_indoor;
    uint16_t     temp_reg_humidity;
    uint16_t     temp_reg_outdoor;
    int16_t      temp_setpoint_c10;   /* setpoint x10, e.g. 260 = 26.0 */

    /* ---- media ---- */
    uint16_t clip_seconds;            /* auto-record clip length       */
    bool     ring_delete_oldest;      /* reclaim space when card fills */
    uint8_t  min_free_percent;

    app_switch_cfg_t switches[APP_SWITCH_COUNT];
} app_settings_t;

/** Loads from NVS, or installs defaults on first boot / version change. */
esp_err_t app_settings_init(void);

/** The live settings. Read freely; call app_settings_commit() after writing. */
app_settings_t *app_settings(void);

/** Persists the current struct and posts APP_EVT_SETTINGS_CHANGED. */
esp_err_t app_settings_commit(void);

/**
 * Schedules a commit a few seconds out, collapsing a burst of changes into
 * one flash write. Use this for anything a finger can repeat quickly --
 * switch taps, a brightness slider drag -- so the NVS partition is not worn
 * down one toggle at a time.
 */
esp_err_t app_settings_commit_deferred(void);

/** Restores defaults, persists them and posts the change event. */
esp_err_t app_settings_factory_reset(void);

#ifdef __cplusplus
}
#endif
