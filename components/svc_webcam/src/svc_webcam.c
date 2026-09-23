#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_net.h"
#include "app_settings.h"
#include "svc_media.h"
#include "svc_webcam.h"

static const char *TAG = "webcam";

/* Frames above this are dropped rather than truncated. A VGA/SVGA JPEG at
 * the quality this firmware uses is well under it. */
#define JPEG_MAX        (96 * 1024)

/* Any constant string works as a boundary as long as it cannot appear in the
 * payload; this is the one the ESP32-CAM examples use, so tools that expect
 * it are happy. */
#define BOUNDARY        "123456789000000000000987654321"
#define STREAM_TYPE     "multipart/x-mixed-replace;boundary=" BOUNDARY
#define STREAM_SEP      "\r\n--" BOUNDARY "\r\n"
#define STREAM_PART     "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

/* No frame for this long and the stream gives up rather than holding the one
 * handler task open forever. */
#define FRAME_WAIT_MS   5000

static const char k_index_html[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Panel camera</title>"
    "<style>body{margin:0;background:#111;display:flex;align-items:center;"
    "justify-content:center;height:100vh}img{max-width:100%;max-height:100vh}"
    "</style></head><body><img src=\"/stream\" alt=\"camera\"></body></html>";

static httpd_handle_t    s_httpd;
static SemaphoreHandle_t s_lock;

static uint8_t  *s_latest;          /* newest frame from the pump   */
static size_t    s_latest_len;
static uint32_t  s_latest_seq;

static uint8_t  *s_send;            /* the streamer's private copy  */

static volatile bool     s_streaming;
static volatile uint32_t s_frames_sent;

/* --------------------------------------------------------------------------
 * Frame intake
 * ------------------------------------------------------------------------ */
/** Runs on the media pump task: copy and release, never block on a socket. */
static void frame_cb(const uint8_t *jpeg, size_t len, void *ctx)
{
    (void)ctx;

    if (len > JPEG_MAX) {
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "frames are %u bytes, over the %d-byte limit -- "
                          "lower the camera resolution or quality",
                     (unsigned)len, JPEG_MAX);
        }
        return;
    }

    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;         /* streamer is mid-copy; this frame just gets skipped */
    }
    memcpy(s_latest, jpeg, len);
    s_latest_len = len;
    s_latest_seq++;
    xSemaphoreGive(s_lock);
}

/**
 * Waits for a frame newer than `seq` and copies it into s_send.
 * Returns its length, or 0 on timeout.
 */
static size_t take_frame(uint32_t *seq)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)FRAME_WAIT_MS * 1000;

    while (esp_timer_get_time() < deadline) {
        size_t len = 0;

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_latest_seq != *seq && s_latest_len) {
                memcpy(s_send, s_latest, s_latest_len);
                len  = s_latest_len;
                *seq = s_latest_seq;
            }
            xSemaphoreGive(s_lock);
        }

        if (len) {
            return len;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Handlers
 * ------------------------------------------------------------------------ */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, k_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t jpg_handler(httpd_req_t *req)
{
    uint32_t     seq = 0;
    const size_t len = take_frame(&seq);

    if (!len) {
        /* Set the status by string rather than through httpd_err_code_t: that
         * enum has no 503, and a wrong code here is a compile error. */
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "no frame", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=frame.jpg");
    return httpd_resp_send(req, (const char *)s_send, len);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (s_streaming) {
        /* Refused rather than queued: the handler task is busy with the first
         * viewer and this request would simply hang until that one left. */
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "one viewer at a time", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    ESP_RETURN_ON_FALSE(httpd_resp_set_type(req, STREAM_TYPE) == ESP_OK,
                        ESP_FAIL, TAG, "content type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    s_streaming = true;
    ESP_LOGI(TAG, "viewer connected");

    uint32_t  seq = 0;
    esp_err_t err = ESP_OK;
    char      part[80];

    while (err == ESP_OK) {
        const size_t len = take_frame(&seq);
        if (!len) {
            ESP_LOGW(TAG, "no frames for %d ms -- closing the stream",
                     FRAME_WAIT_MS);
            break;
        }

        const int hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)len);

        /* Chunked transfer underneath multipart is what every browser and
         * VLC expects here, and is what httpd gives us for free. */
        err = httpd_resp_send_chunk(req, STREAM_SEP, strlen(STREAM_SEP));
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, part, hlen);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)s_send, len);
        }
        if (err == ESP_OK) {
            s_frames_sent++;
        }
    }

    s_streaming = false;
    ESP_LOGI(TAG, "viewer gone after %u frame(s)", (unsigned)s_frames_sent);

    /* The socket is usually already dead by here, so the result is noise. */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */
esp_err_t svc_webcam_start(void)
{
    if (s_httpd) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_latest && s_send, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");

    const app_settings_t *cfg = app_settings();

    httpd_config_t hc  = HTTPD_DEFAULT_CONFIG();
    hc.server_port     = cfg->webcam_port ? cfg->webcam_port : 81;
    /* The control socket must not collide with any other httpd instance, and
     * it is a UDP port unrelated to server_port -- offsetting it keeps them
     * distinct if a second server is ever added. */
    hc.ctrl_port       = 32768 + hc.server_port;
    hc.max_uri_handlers = 4;
    hc.max_open_sockets = 3;
    hc.lru_purge_enable = true;
    hc.stack_size       = 6144;
    /* A viewer that stops reading must not wedge the one handler task. */
    hc.send_wait_timeout = 5;
    hc.recv_wait_timeout = 5;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hc), TAG, "httpd");

    static const httpd_uri_t index_uri  = { .uri = "/",       .method = HTTP_GET, .handler = index_handler };
    static const httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
    static const httpd_uri_t jpg_uri    = { .uri = "/jpg",    .method = HTTP_GET, .handler = jpg_handler };

    httpd_register_uri_handler(s_httpd, &index_uri);
    httpd_register_uri_handler(s_httpd, &stream_uri);
    httpd_register_uri_handler(s_httpd, &jpg_uri);

    s_frames_sent = 0;

    /* Subscribing is also what makes the media pump run, so the camera is
     * only capturing for this while the server is actually up. */
    ESP_RETURN_ON_ERROR(svc_media_add_frame_cb(frame_cb, NULL), TAG, "subscribe");

    char url[64];
    svc_webcam_url(url, sizeof(url));
    ESP_LOGI(TAG, "serving %s", url);
    return ESP_OK;
}

esp_err_t svc_webcam_stop(void)
{
    if (!s_httpd) {
        return ESP_OK;
    }

    svc_media_remove_frame_cb(frame_cb);
    httpd_stop(s_httpd);
    s_httpd     = NULL;
    s_streaming = false;

    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

bool svc_webcam_running(void)
{
    return s_httpd != NULL;
}

bool svc_webcam_streaming(void)
{
    return s_streaming;
}

uint32_t svc_webcam_frames_sent(void)
{
    return s_frames_sent;
}

void svc_webcam_url(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    if (!s_httpd) {
        strlcpy(out, "server stopped", out_len);
        return;
    }

    app_wifi_state_t net;
    app_net_get_state(&net);

    if (!app_net_is_connected() || net.ip[0] == '\0') {
        strlcpy(out, "waiting for Wi-Fi", out_len);
        return;
    }

    const app_settings_t *cfg = app_settings();
    snprintf(out, out_len, "http://%s:%u/stream", net.ip,
             (unsigned)(cfg->webcam_port ? cfg->webcam_port : 81));
}

esp_err_t svc_webcam_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    s_latest = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_SPIRAM);
    s_send   = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_latest && s_send, ESP_ERR_NO_MEM, TAG, "buffers");

    if (app_settings()->webcam_enabled) {
        return svc_webcam_start();
    }

    ESP_LOGI(TAG, "ready, not started (enable it in Settings > Camera)");
    return ESP_OK;
}
