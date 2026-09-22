/* Minimal MJPEG-in-AVI muxer and demuxer.
 *
 * AVI is used rather than a raw JPEG stream because it is the one container
 * every desktop player opens without a codec pack, and because its idx1
 * index makes seeking on the device cheap: playback does not have to walk
 * the whole file to find frame N.
 *
 * Only what MJPEG needs is implemented -- one video stream, no audio.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Index entries are buffered in PSRAM and flushed at close. 18000 frames is
 * ten minutes at 30 fps, well past the clip lengths the UI offers. */
#define AVI_MAX_FRAMES   18000

typedef struct {
    FILE     *fp;
    char      path[128];
    uint16_t  width;
    uint16_t  height;
    uint32_t  frame_count;
    uint32_t  total_bytes;      /* payload only */
    uint32_t  max_frame_bytes;
    uint32_t *index;            /* offset|size pairs, 2 words per frame */
    uint32_t  start_ms;
    bool      open;
} avi_writer_t;

typedef struct {
    FILE     *fp;
    uint16_t  width;
    uint16_t  height;
    uint32_t  frame_count;
    uint32_t  us_per_frame;
    uint32_t *index;            /* offset|size pairs */
    uint32_t  indexed;
    bool      open;
} avi_reader_t;

/* ---- writing ---------------------------------------------------------- */

/** Creates `path` and reserves the header. */
esp_err_t avi_writer_open(avi_writer_t *w, const char *path,
                          uint16_t width, uint16_t height);

/** Appends one JPEG frame. */
esp_err_t avi_writer_add_frame(avi_writer_t *w, const uint8_t *jpeg, size_t len);

/**
 * Writes the index, patches every size field and closes the file. The
 * measured wall-clock duration sets the frame rate, so a stream that ran
 * slower than requested still plays back at the right speed.
 */
esp_err_t avi_writer_close(avi_writer_t *w);

/** Seconds of video written so far. */
uint32_t avi_writer_elapsed_s(const avi_writer_t *w);

/* ---- reading ---------------------------------------------------------- */

esp_err_t avi_reader_open(avi_reader_t *r, const char *path);

/**
 * Reads frame `n` into `buf`. Returns ESP_ERR_INVALID_SIZE (and the needed
 * length in *out_len) when the buffer is too small.
 */
esp_err_t avi_reader_frame(avi_reader_t *r, uint32_t n,
                           uint8_t *buf, size_t buf_len, size_t *out_len);

/** Largest frame in the file, for sizing a decode buffer once. */
uint32_t avi_reader_max_frame_bytes(const avi_reader_t *r);

void avi_reader_close(avi_reader_t *r);

#ifdef __cplusplus
}
#endif
