/* Power system monitor and control.
 *
 * Readings come from an energy meter on the Modbus master; the four rails
 * (main / solar inverter / battery / UPS) are Modbus coils. The register and
 * coil addresses are all in settings so an installer can retarget the panel
 * at a different meter without a rebuild.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Samples kept for the usage chart: 24 h at one point per 15 min. */
#define SVC_POWER_HISTORY   96

typedef enum {
    POWER_RAIL_MAIN = 0,
    POWER_RAIL_SOLAR,
    POWER_RAIL_BATTERY,
    POWER_RAIL_UPS,
    POWER_RAIL_COUNT,
} power_rail_t;

/** Payload of APP_EVT_POWER_UPDATE. Fixed-point to keep the event small. */
typedef struct {
    bool     valid;
    uint16_t voltage_v10;      /* volts  x10  */
    uint16_t current_a100;     /* amps   x100 */
    uint32_t power_w;          /* watts       */
    uint32_t energy_wh;        /* cumulative  */
    bool     rail[POWER_RAIL_COUNT];
    uint8_t  battery_percent;  /* from the on-board PMU, 0 when unknown */
} power_status_t;

/** Starts the poll task. Harmless when no power source is configured. */
esp_err_t svc_power_init(void);

/** Latest sample. */
void svc_power_get(power_status_t *out);

/** Drives one rail's coil and updates the cached state. */
esp_err_t svc_power_set_rail(power_rail_t rail, bool on);

/**
 * Copies the usage history oldest-first into `out` and returns how many
 * points were written (up to SVC_POWER_HISTORY).
 */
size_t svc_power_history(uint32_t *out, size_t max);

#ifdef __cplusplus
}
#endif
