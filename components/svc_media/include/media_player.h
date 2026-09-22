/* AVI playback.
 *
 * A dedicated task walks the file, decodes each JPEG to RGB565 and hands it
 * to the same kind of callback the live preview uses, so the playback page
 * can reuse the live view's image widget wholesale.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "svc_media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} player_state_t;

typedef struct {
    player_state_t state;
    uint32_t       frame;
    uint32_t       frame_count;
    uint32_t       position_s;
    uint32_t       duration_s;
    uint16_t       width;
    uint16_t       height;
} player_status_t;

esp_err_t media_player_init(void);

/** Opens `path` and starts playing from the first frame. */
esp_err_t media_player_open(const char *path);

esp_err_t media_player_play(void);
esp_err_t media_player_pause(void);
esp_err_t media_player_toggle(void);
esp_err_t media_player_stop(void);

/** Jumps to a fraction of the clip, 0..1000 (per mille). */
esp_err_t media_player_seek_permille(uint16_t pos);

/** Steps one frame in either direction while paused. */
esp_err_t media_player_step(int delta);

void media_player_status(player_status_t *out);

/** Decoded frames are delivered here, exactly like the live preview. */
void media_player_set_frame_cb(media_preview_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
