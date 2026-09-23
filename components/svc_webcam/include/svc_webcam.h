/* The camera, served over HTTP as multipart MJPEG.
 *
 *   GET http://<panel-ip>:81/stream    multipart/x-mixed-replace
 *   GET http://<panel-ip>:81/jpg       one JPEG
 *   GET http://<panel-ip>:81/          a page that just embeds the stream
 *
 * The stream is the same JPEG the sensor produces and the recorder stores --
 * nothing is re-encoded, so serving it costs a memcpy and a socket write.
 *
 * One viewer at a time. A streaming handler never returns, and esp_http_server
 * runs its handlers on one task, so a second stream would block behind the
 * first; the second request is refused with 503 rather than left hanging.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocates buffers and starts the server when settings.webcam_enabled is
 * set. Call after svc_media_init() and app_net_init().
 */
esp_err_t svc_webcam_init(void);

esp_err_t svc_webcam_start(void);
esp_err_t svc_webcam_stop(void);
bool      svc_webcam_running(void);

/** True while a client is pulling /stream. */
bool svc_webcam_streaming(void);

/** Frames written to the network since the server last started. */
uint32_t svc_webcam_frames_sent(void);

/**
 * The stream URL, e.g. "http://192.168.1.50:81/stream".
 *
 * Writes the reason instead when there is nowhere to serve from -- no Wi-Fi,
 * or the server is stopped -- so the UI can show one field either way.
 */
void svc_webcam_url(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
