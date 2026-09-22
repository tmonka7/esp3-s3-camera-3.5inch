/* The serial console behind the UART page.
 *
 * Owns one hardware UART, reassembles incoming bytes into display lines and
 * publishes them as APP_EVT_UART_RX. Optionally tees everything to
 * /sdcard/logs/uart-<date>.log.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SVC_UART_LINE_MAX   96

typedef struct {
    char     stamp[16];              /* "10:24:12"                  */
    uint16_t len;
    bool     is_tx;                  /* true for locally sent data  */
    char     text[SVC_UART_LINE_MAX];
} svc_uart_line_t;

typedef struct {
    uint32_t baud;
    uint8_t  databits;               /* 5..8                        */
    uint8_t  parity;                 /* 0 none, 1 odd, 2 even       */
    uint8_t  stopbits;               /* 1 or 2                      */
} svc_uart_cfg_t;

typedef struct {
    bool     open;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_lines;
    uint32_t errors;
} svc_uart_stats_t;

/** Allocates buffers and the RX task. Does not open the port. */
esp_err_t svc_uart_init(void);

/** Applies `cfg` (NULL = whatever is in settings) and opens the port. */
esp_err_t svc_uart_open(const svc_uart_cfg_t *cfg);
esp_err_t svc_uart_close(void);
bool      svc_uart_is_open(void);

/** Sends raw bytes; echoed back as an APP_EVT_UART_RX line with is_tx set. */
esp_err_t svc_uart_send(const void *data, size_t len);

/** Sends `text`, appending CRLF. */
esp_err_t svc_uart_send_line(const char *text);

/** Parses "01 A2 FF" and sends the bytes. */
esp_err_t svc_uart_send_hex(const char *hex_text);

void svc_uart_get_stats(svc_uart_stats_t *out);
void svc_uart_clear_stats(void);

/** Writes the in-RAM scrollback to the TF card. */
esp_err_t svc_uart_save_log(char *out_path, size_t out_path_len);

/** Number of lines currently in scrollback, newest last. */
size_t svc_uart_history_count(void);

/** Copies scrollback entry `index` (0 = oldest). */
bool svc_uart_history_get(size_t index, svc_uart_line_t *out);

void svc_uart_history_clear(void);

#ifdef __cplusplus
}
#endif
