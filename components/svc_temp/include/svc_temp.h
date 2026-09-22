/* Indoor / outdoor climate.
 *
 * Indoor readings come from an SHT3x on the shared I2C bus when one is
 * fitted, otherwise from Modbus holding registers. Outdoor is Modbus-only.
 * Per-room values are kept so the room tabs on the temperature page have
 * something to show even when only one sensor exists.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SVC_TEMP_ROOMS   4

/** Payload of APP_EVT_TEMP_UPDATE. Temperatures are x10 degrees Celsius. */
typedef struct {
    bool    indoor_valid;
    int16_t indoor_c10;
    uint8_t humidity_pct;

    bool    outdoor_valid;
    int16_t outdoor_c10;

    int16_t setpoint_c10;

    bool    room_valid[SVC_TEMP_ROOMS];
    int16_t room_c10[SVC_TEMP_ROOMS];
} temp_status_t;

esp_err_t svc_temp_init(void);

void svc_temp_get(temp_status_t *out);

/** Adjusts the setpoint by `delta_c10` (e.g. +5 for +0.5 degrees). */
esp_err_t svc_temp_nudge_setpoint(int16_t delta_c10);

esp_err_t svc_temp_set_setpoint(int16_t c10);

const char *svc_temp_room_name(int index);

#ifdef __cplusplus
}
#endif
