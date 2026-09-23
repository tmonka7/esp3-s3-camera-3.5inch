/* Local Binary Pattern Histograms.
 *
 * LBPH is the classical face descriptor: label every pixel by how its eight
 * neighbours compare to it, histogram those labels over a grid of cells, and
 * compare two faces by comparing their histograms. It learns nothing and
 * needs no trained weights -- the "model" is simply the faces you enrolled,
 * which is what makes it usable on a board with no way to fetch one.
 *
 * Only the 58 "uniform" patterns (at most two circular 0-1 transitions) get
 * their own bin; everything else shares one. Uniform patterns are the edges,
 * corners and flat spots that carry the signal, and the rest is mostly noise,
 * so this cuts the descriptor from 256 bins per cell to 59 at no real cost in
 * accuracy -- the standard Ojala formulation.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_NORM_W     64
#define FACE_NORM_H     64
#define LBP_GRID        4                       /* 4x4 cells of 16x16 px   */
#define LBP_BINS        59                      /* 58 uniform + 1 catch-all */
#define FACE_DESC_LEN   (LBP_GRID * LBP_GRID * LBP_BINS)    /* 944 bytes   */

/** Builds the uniform-pattern lookup table. Call once. */
void face_lbp_init(void);

/**
 * Flattens the histogram of `gray` in place.
 *
 * Doing this before the descriptor is what makes the same face at two
 * different exposures still match: LBP is invariant to a monotonic change in
 * brightness, but not to the contrast collapse of a badly lit frame.
 */
void face_lbp_equalize(uint8_t *gray, int w, int h);

/** FACE_NORM_W x FACE_NORM_H grayscale in, FACE_DESC_LEN descriptor out. */
void face_lbp_descriptor(const uint8_t *gray, uint8_t *desc);

/**
 * Mean per-cell chi-square distance, 0 (identical) to 2 (nothing in common).
 * Same person typically lands near 0.2-0.4, different people near 0.7+.
 */
float face_lbp_distance(const uint8_t *a, const uint8_t *b);

/** Maps a distance onto the 0..100 confidence the rest of the app speaks. */
uint8_t face_lbp_confidence(float distance);

#ifdef __cplusplus
}
#endif
