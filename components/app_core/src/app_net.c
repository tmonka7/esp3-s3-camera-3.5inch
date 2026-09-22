#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "app_events.h"
#include "app_net.h"
#include "app_settings.h"

static const char *TAG = "app_net";

#define RECONNECT_DELAY_MS  5000
#define MAX_FAST_RETRIES    5

static app_wifi_state_t s_state;
static bool             s_started;
static int              s_retries;
static esp_netif_t     *s_netif;

static void publish(void)
{
    app_event_post(APP_EVT_WIFI_STATE, &s_state, sizeof(s_state));
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        s_state.state = APP_LINK_CONNECTING;
        publish();
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        s_state.state = APP_LINK_CONNECTING;
        s_state.ip[0] = '\0';
        publish();

        /* Retry hard a few times (AP rebooting), then back off so a wrong
         * password does not keep the radio busy forever. */
        if (s_retries < MAX_FAST_RETRIES) {
            s_retries++;
            esp_wifi_connect();
        } else {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            esp_wifi_connect();
        }
        break;
    }

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)data;
    snprintf(s_state.ip, sizeof(s_state.ip), IPSTR, IP2STR(&evt->ip_info.ip));
    s_state.state = APP_LINK_UP;
    s_retries     = 0;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_state.rssi = ap.rssi;
        strlcpy(s_state.ssid, (const char *)ap.ssid, sizeof(s_state.ssid));
    }

    ESP_LOGI(TAG, "connected to %s, ip %s, rssi %d", s_state.ssid, s_state.ip, s_state.rssi);
    publish();
}

esp_err_t app_net_init(void)
{
    if (!app_settings()->wifi_enabled) {
        ESP_LOGI(TAG, "Wi-Fi disabled in settings");
        s_state.state = APP_LINK_DOWN;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    /* The default system loop is separate from the app loop and is what the
     * Wi-Fi and lwIP stacks post to. */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "default loop");
    }

    s_netif = esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL),
                        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL),
                        TAG, "ip handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");

    s_started = true;
    return app_net_reconnect();
}

esp_err_t app_net_reconnect(void)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "wifi not initialised");

    const app_settings_t *cfg = app_settings();
    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no SSID configured");
        s_state.state = APP_LINK_DOWN;
        publish();
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid,     cfg->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg->wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    strlcpy(s_state.ssid, cfg->wifi_ssid, sizeof(s_state.ssid));
    s_retries = 0;

    esp_wifi_stop();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "sta config");
    return esp_wifi_start();
}

esp_err_t app_net_stop(void)
{
    if (!s_started) {
        return ESP_OK;
    }
    s_state.state = APP_LINK_DOWN;
    s_state.ip[0] = '\0';
    publish();
    return esp_wifi_stop();
}

void app_net_get_state(app_wifi_state_t *out)
{
    if (out) {
        *out = s_state;
    }
}

bool app_net_is_connected(void)
{
    return s_state.state == APP_LINK_UP;
}

void *app_net_netif(void)
{
    return (void *)s_netif;
}
