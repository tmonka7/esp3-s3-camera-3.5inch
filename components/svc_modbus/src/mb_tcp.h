#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Records the target. The socket is opened lazily on the first request. */
esp_err_t mb_tcp_start(const char *host, uint16_t port);
esp_err_t mb_tcp_stop(void);
bool      mb_tcp_is_up(void);

/** True once a socket to the gateway is actually established. */
bool mb_tcp_is_connected(void);

esp_err_t mb_tcp_transact(uint8_t unit, uint8_t fn, uint16_t addr,
                          uint16_t count, const uint16_t *wr_data, void *out);

#ifdef __cplusplus
}
#endif
