#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_check.h"

#include "bsp_board.h"
#include "bsp_storage.h"
#include "app_events.h"
#include "app_settings.h"
#include "app_time.h"
#include "svc_uart.h"

static const char *TAG = "svc_uart";

#define RX_BUF_SIZE     4096
#define TX_BUF_SIZE     2048
#define HISTORY_MAX     200
/* A line is emitted on newline, when it fills up, or when the link has been
 * quiet this long -- otherwise binary protocols would never display. */
#define IDLE_FLUSH_MS   120

static bool              s_open;
static bool              s_running;
static TaskHandle_t      s_task;
static SemaphoreHandle_t s_lock;
static svc_uart_stats_t  s_stats;

/* Ring buffer of display lines. */
static svc_uart_line_t  *s_history;
static size_t            s_hist_head;    /* next write slot */
static size_t            s_hist_count;

static void history_push(const svc_uart_line_t *line)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_history[s_hist_head] = *line;
    s_hist_head = (s_hist_head + 1) % HISTORY_MAX;
    if (s_hist_count < HISTORY_MAX) {
        s_hist_count++;
    }
    xSemaphoreGive(s_lock);
}

size_t svc_uart_history_count(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = s_hist_count;
    xSemaphoreGive(s_lock);
    return n;
}

bool svc_uart_history_get(size_t index, svc_uart_line_t *out)
{
    if (!out) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = index < s_hist_count;
    if (ok) {
        const size_t oldest = (s_hist_head + HISTORY_MAX - s_hist_count) % HISTORY_MAX;
        *out = s_history[(oldest + index) % HISTORY_MAX];
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void svc_uart_history_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_hist_head  = 0;
    s_hist_count = 0;
    xSemaphoreGive(s_lock);
}

/* --------------------------------------------------------------------------
 * Line assembly
 * ------------------------------------------------------------------------ */
static void render_hex(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 3 < out_len; i++) {
        pos += (size_t)snprintf(out + pos, out_len - pos, "%02X ", data[i]);
    }
    if (pos > 0 && out[pos - 1] == ' ') {
        out[pos - 1] = '\0';
    }
}

static void render_ascii(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 1 < out_len; i++) {
        /* Keep the display single-line and printable; everything else becomes
         * a dot so a stray 0x00 cannot truncate the row. */
        out[pos++] = isprint(data[i]) ? (char)data[i] : '.';
    }
    out[pos] = '\0';
}

static void emit(const uint8_t *data, size_t len, bool is_tx)
{
    if (len == 0) {
        return;
    }

    svc_uart_line_t line = { .len = (uint16_t)len, .is_tx = is_tx };
    app_time_format(line.stamp, sizeof(line.stamp), "%H:%M:%S");

    if (app_settings()->uart_hex_view) {
        render_hex(data, len, line.text, sizeof(line.text));
    } else {
        render_ascii(data, len, line.text, sizeof(line.text));
    }

    history_push(&line);
    s_stats.rx_lines++;
    app_event_post(APP_EVT_UART_RX, &line, sizeof(line));
}

/* --------------------------------------------------------------------------
 * RX task
 * ------------------------------------------------------------------------ */
static void rx_task(void *arg)
{
    (void)arg;

    uint8_t *chunk = malloc(256);
    uint8_t *line  = malloc(SVC_UART_LINE_MAX);
    size_t   line_len = 0;
    TickType_t last_rx = xTaskGetTickCount();

    if (!chunk || !line) {
        ESP_LOGE(TAG, "out of memory");
        free(chunk);
        free(line);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    while (s_running) {
        if (!s_open) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const int n = uart_read_bytes(BSP_UART_NUM, chunk, 256, pdMS_TO_TICKS(20));
        if (n > 0) {
            s_stats.rx_bytes += (uint32_t)n;
            last_rx = xTaskGetTickCount();

            for (int i = 0; i < n; i++) {
                const uint8_t b = chunk[i];

                if (b == '\n' || line_len == SVC_UART_LINE_MAX - 1) {
                    emit(line, line_len, false);
                    line_len = 0;
                    if (b == '\n') {
                        continue;
                    }
                }
                if (b == '\r') {
                    continue;
                }
                line[line_len++] = b;
            }
        } else if (line_len > 0 &&
                   (xTaskGetTickCount() - last_rx) > pdMS_TO_TICKS(IDLE_FLUSH_MS)) {
            emit(line, line_len, false);
            line_len = 0;
        }
    }

    free(chunk);
    free(line);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */
esp_err_t svc_uart_init(void)
{
    if (s_history) {
        return ESP_OK;
    }

    s_history = calloc(HISTORY_MAX, sizeof(svc_uart_line_t));
    ESP_RETURN_ON_FALSE(s_history, ESP_ERR_NO_MEM, TAG, "history alloc");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    s_running = true;
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(rx_task, "uart_rx", 4096, NULL, 6,
                                                &s_task, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "rx task");
    return ESP_OK;
}

static uart_parity_t to_parity(uint8_t p)
{
    switch (p) {
    case 1:  return UART_PARITY_ODD;
    case 2:  return UART_PARITY_EVEN;
    default: return UART_PARITY_DISABLE;
    }
}

static uart_word_length_t to_databits(uint8_t d)
{
    switch (d) {
    case 5:  return UART_DATA_5_BITS;
    case 6:  return UART_DATA_6_BITS;
    case 7:  return UART_DATA_7_BITS;
    default: return UART_DATA_8_BITS;
    }
}

esp_err_t svc_uart_open(const svc_uart_cfg_t *cfg)
{
    if (s_open) {
        svc_uart_close();
    }

    const app_settings_t *st = app_settings();
    const svc_uart_cfg_t fallback = {
        .baud     = st->uart_baud,
        .databits = st->uart_databits,
        .parity   = st->uart_parity,
        .stopbits = st->uart_stopbits,
    };
    const svc_uart_cfg_t *c = cfg ? cfg : &fallback;

    const uart_config_t uc = {
        .baud_rate = (int)c->baud,
        .data_bits = to_databits(c->databits),
        .parity    = to_parity(c->parity),
        .stop_bits = (c->stopbits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(BSP_UART_NUM, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "driver install: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(uart_param_config(BSP_UART_NUM, &uc), TAG, "param");
    ESP_RETURN_ON_ERROR(uart_set_pin(BSP_UART_NUM, BSP_UART_PIN_TX, BSP_UART_PIN_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "pins");
    uart_flush_input(BSP_UART_NUM);

    s_open = true;
    s_stats.open = true;

    const app_link_state_t link = APP_LINK_UP;
    app_event_post(APP_EVT_UART_STATE, &link, sizeof(link));
    ESP_LOGI(TAG, "UART%d open @ %lu %u%c%u", BSP_UART_NUM, (unsigned long)c->baud,
             c->databits, "NOE"[c->parity <= 2 ? c->parity : 0], c->stopbits);
    return ESP_OK;
}

esp_err_t svc_uart_close(void)
{
    if (!s_open) {
        return ESP_OK;
    }
    s_open       = false;
    s_stats.open = false;

    esp_err_t err = uart_driver_delete(BSP_UART_NUM);

    const app_link_state_t link = APP_LINK_DOWN;
    app_event_post(APP_EVT_UART_STATE, &link, sizeof(link));
    return err;
}

bool svc_uart_is_open(void)
{
    return s_open;
}

esp_err_t svc_uart_send(const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_open, ESP_ERR_INVALID_STATE, TAG, "port closed");
    ESP_RETURN_ON_FALSE(data && len, ESP_ERR_INVALID_ARG, TAG, "empty");

    const int written = uart_write_bytes(BSP_UART_NUM, data, len);
    if (written < 0) {
        s_stats.errors++;
        return ESP_FAIL;
    }
    s_stats.tx_bytes += (uint32_t)written;

    emit((const uint8_t *)data, len, true);
    return ESP_OK;
}

esp_err_t svc_uart_send_line(const char *text)
{
    ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "null");

    char buf[SVC_UART_LINE_MAX + 2];
    const int n = snprintf(buf, sizeof(buf), "%s\r\n", text);
    return svc_uart_send(buf, (size_t)(n > 0 ? n : 0));
}

esp_err_t svc_uart_send_hex(const char *hex_text)
{
    ESP_RETURN_ON_FALSE(hex_text, ESP_ERR_INVALID_ARG, TAG, "null");

    uint8_t bytes[SVC_UART_LINE_MAX / 2];
    size_t  count = 0;
    int     nibble = -1;

    for (const char *p = hex_text; *p && count < sizeof(bytes); p++) {
        int v;
        if (isdigit((unsigned char)*p))            v = *p - '0';
        else if (*p >= 'a' && *p <= 'f')           v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')           v = *p - 'A' + 10;
        else                                       { nibble = -1; continue; }

        if (nibble < 0) {
            nibble = v;
        } else {
            bytes[count++] = (uint8_t)((nibble << 4) | v);
            nibble = -1;
        }
    }

    ESP_RETURN_ON_FALSE(count, ESP_ERR_INVALID_ARG, TAG, "no hex bytes in input");
    return svc_uart_send(bytes, count);
}

void svc_uart_get_stats(svc_uart_stats_t *out)
{
    if (out) {
        *out = s_stats;
    }
}

void svc_uart_clear_stats(void)
{
    const bool open = s_stats.open;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.open = open;
}

esp_err_t svc_uart_save_log(char *out_path, size_t out_path_len)
{
    ESP_RETURN_ON_FALSE(bsp_storage_mounted(), ESP_ERR_NOT_FOUND, TAG, "no TF card");

    char stamp[24];
    app_time_stamp_filename(stamp, sizeof(stamp));

    char path[96];
    snprintf(path, sizeof(path), BSP_SD_MOUNT_POINT "/logs/uart-%s.log", stamp);

    FILE *f = fopen(path, "w");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "cannot open %s", path);

    const size_t n = svc_uart_history_count();
    for (size_t i = 0; i < n; i++) {
        svc_uart_line_t line;
        if (svc_uart_history_get(i, &line)) {
            fprintf(f, "[%s] %s %s\n", line.stamp, line.is_tx ? "TX" : "RX", line.text);
        }
    }
    fclose(f);

    if (out_path && out_path_len) {
        strlcpy(out_path, path, out_path_len);
    }
    ESP_LOGI(TAG, "saved %u lines to %s", (unsigned)n, path);
    return ESP_OK;
}
