/* Finding a face in a frame, without a trained detector.
 *
 * A cascade or a CNN would be better and both need a model file. What is
 * left that needs nothing is chrominance: in YCbCr, skin occupies a narrow,
 * well-documented band of Cb/Cr that holds across skin tones, because tone
 * mostly lives in Y. Segment on that, take the largest region whose shape
 * could be a head, and you have a usable face box.
 *
 * Where this falls down, honestly: wood, sand, terracotta and some clothing
 * land in the same band, so it needs the shape test to stay sane, and it is
 * sensitive to the colour temperature of the light. It finds a face, it does
 * not verify that one is there -- that is what the recogniser is for.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Allocates the working grid. Call once, after PSRAM is up. */
esp_err_t face_locate_init(void);

/**
 * Finds the most face-like skin region in a big-endian RGB565 frame.
 *
 * Returns false when nothing qualifies, which is the common case and not an
 * error. The box is in `rgb` pixel coordinates.
 */
bool face_locate(const uint8_t *rgb, int w, int h,
                 uint16_t *fx, uint16_t *fy, uint16_t *fw, uint16_t *fh);

/**
 * Crops the box out of `rgb`, converts to grayscale and scales it to
 * FACE_NORM_W x FACE_NORM_H with nearest-neighbour sampling.
 */
void face_crop_gray(const uint8_t *rgb, int w, int h,
                    uint16_t fx, uint16_t fy, uint16_t fw, uint16_t fh,
                    uint8_t *out_gray);

#ifdef __cplusplus
}
#endif
