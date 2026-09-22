/* Modbus master.
 *
 * esp-modbus v1.x hosts a single master instance at a time, so RTU and TCP
 * are two transports the user switches between on the Modbus page rather
 * than two simultaneously live stacks. Switching tears the stack down and
 * brings it back up, which takes a few hundred milliseconds.
 *
 * Every request goes through one mutex: the UI, the power poller, the
 * temperature poller and the switch service all share the master.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SVC_MODBUS_MAX_REGS   64

typedef enum {
    MB_TRANSPORT_NONE = 0,
    MB_TRANSPORT_RTU,
    MB_TRANSPORT_TCP,
} svc_modbus_transport_t;

/** The subset of Modbus functions the UI exposes. */
typedef enum {
    MB_FN_READ_COILS          = 1,
    MB_FN_READ_DISCRETE       = 2,
    MB_FN_READ_HOLDING        = 3,
    MB_FN_READ_INPUT          = 4,
    MB_FN_WRITE_SINGLE_COIL   = 5,
    MB_FN_WRITE_SINGLE_REG    = 6,
    MB_FN_WRITE_MULTI_COILS   = 15,
    MB_FN_WRITE_MULTI_REGS    = 16,
} svc_modbus_fn_t;

/** Result of the last UI-issued transaction, for APP_EVT_MODBUS_RESULT. */
typedef struct {
    bool     ok;
    uint8_t  slave;
    uint8_t  function;
    uint16_t address;
    uint16_t count;
    uint16_t values[SVC_MODBUS_MAX_REGS];
    int      err;                    /* esp_err_t when !ok */
} svc_modbus_result_t;

esp_err_t svc_modbus_init(void);
esp_err_t svc_modbus_deinit(void);

/** Tears down the current stack and brings the requested one up. */
esp_err_t svc_modbus_set_transport(svc_modbus_transport_t t);
svc_modbus_transport_t svc_modbus_get_transport(void);
app_link_state_t svc_modbus_link_state(void);

/* ---- synchronous primitives (safe from any task) ---------------------- */

/** Reads holding (fn 3) or input (fn 4) registers into `out`. */
esp_err_t svc_modbus_read_regs(uint8_t slave, svc_modbus_fn_t fn,
                               uint16_t addr, uint16_t count, uint16_t *out);

/** Reads coils (fn 1) or discrete inputs (fn 2); one byte per 8 points. */
esp_err_t svc_modbus_read_bits(uint8_t slave, svc_modbus_fn_t fn,
                               uint16_t addr, uint16_t count, uint8_t *out);

esp_err_t svc_modbus_write_reg(uint8_t slave, uint16_t addr, uint16_t value);
esp_err_t svc_modbus_write_regs(uint8_t slave, uint16_t addr,
                                uint16_t count, const uint16_t *values);
esp_err_t svc_modbus_write_coil(uint8_t slave, uint16_t addr, bool on);

/**
 * Runs a transaction on behalf of the Modbus page and publishes the outcome
 * as APP_EVT_MODBUS_RESULT. Never blocks the caller for longer than the
 * configured response timeout.
 */
esp_err_t svc_modbus_ui_request(uint8_t slave, svc_modbus_fn_t fn,
                                uint16_t addr, uint16_t count,
                                const uint16_t *write_values);

/** Transaction counters for the status strip. */
void svc_modbus_get_stats(uint32_t *ok, uint32_t *failed);

#ifdef __cplusplus
}
#endif
