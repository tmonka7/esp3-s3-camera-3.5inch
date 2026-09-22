/* The ten room switches.
 *
 * Each switch is bound independently: a local GPIO, a TCA9554 bit, a Modbus
 * coil, or nothing at all (UI-only). Switch 10 ("Light_All") is treated as a
 * scene: driving it drives every switch bound to a real output.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Payload of APP_EVT_SWITCH_UPDATE. */
typedef struct {
    uint8_t index;           /* 0..APP_SWITCH_COUNT-1 */
    bool    state;
    bool    ok;              /* false when the write to the output failed */
} switch_update_t;

/** Restores the saved states onto the hardware. */
esp_err_t svc_switch_init(void);

/** Drives one switch and persists the new state. */
esp_err_t svc_switch_set(uint8_t index, bool on);

/** Convenience for the UI's tap handler. */
esp_err_t svc_switch_toggle(uint8_t index);

bool svc_switch_get(uint8_t index);

/** Drives every switch that has a real output binding. */
esp_err_t svc_switch_set_all(bool on);

const char *svc_switch_name(uint8_t index);

/**
 * Reads back the switches bound to Modbus coils so the UI reflects changes
 * made by other controllers on the bus. Cheap no-op when nothing is bound.
 */
esp_err_t svc_switch_refresh_from_bus(void);

#ifdef __cplusplus
}
#endif
