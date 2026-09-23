#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wi-Fi station. Optional: Modbus-TCP and webhook notifications need
 * it, but the panel is fully usable on RS485 alone with Wi-Fi switched off.
 */
esp_err_t app_net_init(void);

/** Applies the SSID/password currently in settings and (re)connects. */
esp_err_t app_net_reconnect(void);

esp_err_t app_net_stop(void);

/** Last known link state, safe to poll from the UI. */
void app_net_get_state(app_wifi_state_t *out);

bool app_net_is_connected(void);

/** The station netif, which the Modbus-TCP master needs. NULL when Wi-Fi
 *  was never initialised. */
void *app_net_netif(void);

#ifdef __cplusplus
}
#endif
