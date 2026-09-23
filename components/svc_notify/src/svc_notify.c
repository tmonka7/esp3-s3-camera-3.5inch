#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "driver/gpio.h"

#include "bsp_board.h"
#include "bsp_storage.h"
#include "app_settings.h"
#include "app_net.h"
#include "app_time.h"
#include "svc_notify.h"

static const char *TAG = "svc_notify";

#define QUEUE_DEPTH     8
#define HTTP_TIMEOUT_MS 5000

typedef struct {
    char title[48];
    char detail[96];
    char file[80];
} alert_t;

static QueueHandle_t s_queue;
static bool          s_buzzer_ready;

/* --------------------------------------------------------------------------
 * Local outputs
 * ------------------------------------------------------------------------ */
void svc_notify_beep(void)
{
    if (!s_buzzer_ready) {
        return;
    }
    gpio_set_level((gpio_num_t)BSP_BUZZER_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(60));
    gpio_set_level((gpio_num_t)BSP_BUZZER_PIN, 0);
}

static void append_event_log(const alert_t *a)
{
    if (!bsp_storage_mounted()) {
        return;
    }

    char date[16];
    app_time_format(date, sizeof(date), "%Y-%m-%d");

    char path[96];
    snprintf(path, sizeof(path), BSP_SD_MOUNT_POINT "/events/%s.csv", date);

    /* One CSV per day keeps files small enough to open on a phone and makes
     * retention a matter of deleting whole files. */
    const bool fresh = (access(path, F_OK) != 0);

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGW(TAG, "cannot append to %s", path);
        return;
    }
    if (fresh) {
        fprintf(f, "time,title,detail,file\n");
    }

    char stamp[24];
    app_time_format(stamp, sizeof(stamp), "%H:%M:%S");
    fprintf(f, "%s,%s,%s,%s\n", stamp, a->title, a->detail, a->file);
    fclose(f);
}

/* --------------------------------------------------------------------------
 * Webhook
 * ------------------------------------------------------------------------ */
static void post_webhook(const alert_t *a)
{
    const app_settings_t *cfg = app_settings();
    if (cfg->notify_webhook[0] == '\0' || !app_net_is_connected()) {
        return;
    }

    char body[320];
    const int n = snprintf(body, sizeof(body),
                           "{\"title\":\"%s\",\"detail\":\"%s\",\"file\":\"%s\"}",
                           a->title, a->detail, a->file);
    if (n <= 0) {
        return;
    }

    const esp_http_client_config_t http_cfg = {
        .url         = cfg->notify_webhook,
        .method      = HTTP_METHOD_POST,
        .timeout_ms  = HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, n);

    const esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "webhook -> HTTP %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGW(TAG, "webhook failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

/* --------------------------------------------------------------------------
 * Task
 * ------------------------------------------------------------------------ */
static void notify_task(void *arg)
{
    (void)arg;

    alert_t a;
    while (xQueueReceive(s_queue, &a, portMAX_DELAY) == pdTRUE) {
        svc_notify_beep();
        append_event_log(&a);
        post_webhook(&a);
    }

    vTaskDelete(NULL);
}

esp_err_t svc_notify_init(void)
{
    if (s_queue) {
        return ESP_OK;
    }

    /* BSP_BUZZER_PIN is a compile-time constant, so the compiler folds this
     * block even when the guard is false -- and with the pin left at -1 the
     * cast produced a shift count of 2^32-1, i.e. a -Wshift-count-overflow
     * error on a branch that can never run. Masking the count to 0..63 keeps
     * the folded constant legal; the guard still decides whether the pin is
     * actually used. */
    const int buzzer_pin = BSP_BUZZER_PIN;
    if (buzzer_pin >= GPIO_NUM_0 && buzzer_pin <= GPIO_NUM_31) {
        const gpio_num_t    pin = (gpio_num_t)buzzer_pin;
        const gpio_config_t io  = {
            .pin_bit_mask = 1ULL << (buzzer_pin & 0x3F),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) == ESP_OK) {
            gpio_set_level(pin, 0);
            s_buzzer_ready = true;
        }
    }

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(alert_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");

    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(notify_task, "notify", 5120, NULL, 3,
                                                NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

esp_err_t svc_notify_alert(const char *title, const char *detail, const char *file_path)
{
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    alert_t a = {0};
    strlcpy(a.title,  title  ? title  : "", sizeof(a.title));
    strlcpy(a.detail, detail ? detail : "", sizeof(a.detail));
    strlcpy(a.file,   file_path ? file_path : "", sizeof(a.file));

    /* Never block the detector: an alert dropped because eight are already
     * queued is better than a stalled camera pipeline. */
    if (xQueueSend(s_queue, &a, 0) != pdTRUE) {
        ESP_LOGW(TAG, "alert queue full, dropping \"%s\"", a.title);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
