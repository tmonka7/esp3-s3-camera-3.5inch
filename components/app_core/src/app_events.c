#include "esp_log.h"
#include "esp_check.h"

#include "app_events.h"

static const char *TAG = "app_events";

ESP_EVENT_DEFINE_BASE(APP_EVENT);

static esp_event_loop_handle_t s_loop;

esp_err_t app_events_init(void)
{
    if (s_loop) {
        return ESP_OK;
    }

    const esp_event_loop_args_t args = {
        .queue_size      = 32,
        .task_name       = "app_evt",
        .task_priority   = 5,
        .task_stack_size = 6144,
        /* Core 0 alongside LVGL: handlers mostly take the LVGL lock, and
         * keeping them off core 1 leaves the camera pipeline undisturbed. */
        .task_core_id    = 0,
    };
    ESP_RETURN_ON_ERROR(esp_event_loop_create(&args, &s_loop), TAG, "loop create");

    ESP_LOGI(TAG, "event loop up");
    return ESP_OK;
}

esp_err_t app_event_post(app_event_id_t id, const void *data, size_t size)
{
    if (!s_loop) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Never block a service task on a full UI queue -- dropping one status
     * refresh is always better than stalling the producer. */
    return esp_event_post_to(s_loop, APP_EVENT, (int32_t)id, data, size, 0);
}

esp_err_t app_event_post_isr(app_event_id_t id, const void *data, size_t size)
{
    if (!s_loop) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t hp_task_woken = pdFALSE;
    esp_err_t err = esp_event_isr_post_to(s_loop, APP_EVENT, (int32_t)id,
                                          data, size, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
    return err;
}

esp_err_t app_event_subscribe(app_event_id_t id, esp_event_handler_t handler, void *arg)
{
    if (!s_loop) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_event_handler_register_with(s_loop, APP_EVENT, (int32_t)id, handler, arg);
}

esp_err_t app_event_subscribe_any(esp_event_handler_t handler, void *arg)
{
    if (!s_loop) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_event_handler_register_with(s_loop, APP_EVENT, ESP_EVENT_ANY_ID, handler, arg);
}

esp_err_t app_event_unsubscribe(app_event_id_t id, esp_event_handler_t handler)
{
    if (!s_loop) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_event_handler_unregister_with(s_loop, APP_EVENT, (int32_t)id, handler);
}
