#include <string.h>

#include "face_lbp.h"

/* Distance that counts as a certain match, and the one that counts as none.
 * These are the two numbers that decide how forgiving recognition is, so they
 * are named rather than buried in the mapping below. */
#define DIST_CERTAIN    0.15f
#define DIST_HOPELESS   0.85f

#define CELL_W          (FACE_NORM_W / LBP_GRID)
#define CELL_H          (FACE_NORM_H / LBP_GRID)

/** 256 raw patterns -> 59 bins. Index 58 is everything non-uniform. */
static uint8_t s_uniform[256];
static bool    s_ready;

void face_lbp_init(void)
{
    if (s_ready) {
        return;
    }

    uint8_t next = 0;
    for (int code = 0; code < 256; code++) {
        /* Count circular 0-1 transitions: rotate by one and compare. */
        const uint8_t rot = (uint8_t)(((code << 1) | (code >> 7)) & 0xFF);
        uint8_t       diff = (uint8_t)(code ^ rot);

        int transitions = 0;
        while (diff) {
            transitions += (diff & 1);
            diff >>= 1;
        }

        s_uniform[code] = (transitions <= 2) ? next++ : (uint8_t)(LBP_BINS - 1);
    }
    /* next lands on 58 for 8-bit patterns; the table is wrong if it does not. */
    s_ready = true;
}

void face_lbp_equalize(uint8_t *gray, int w, int h)
{
    const int total = w * h;
    if (total <= 0) {
        return;
    }

    uint32_t hist[256] = { 0 };
    for (int i = 0; i < total; i++) {
        hist[gray[i]]++;
    }

    /* Map through the cumulative distribution, skipping the leading empty
     * levels so a dark image expands across the whole range rather than
     * being shifted up it. */
    uint32_t cdf      = 0;
    uint32_t cdf_min  = 0;
    bool     have_min = false;
    uint8_t  lut[256];

    for (int i = 0; i < 256; i++) {
        cdf += hist[i];
        if (!have_min && cdf > 0) {
            cdf_min  = cdf;
            have_min = true;
        }
        if ((uint32_t)total == cdf_min) {
            lut[i] = (uint8_t)i;        /* single-valued image: leave it */
        } else {
            const uint32_t num = (cdf - cdf_min) * 255u;
            lut[i] = (uint8_t)(num / ((uint32_t)total - cdf_min));
        }
    }

    for (int i = 0; i < total; i++) {
        gray[i] = lut[gray[i]];
    }
}

void face_lbp_descriptor(const uint8_t *gray, uint8_t *desc)
{
    uint16_t counts[LBP_GRID * LBP_GRID][LBP_BINS];
    memset(counts, 0, sizeof(counts));

    /* The one-pixel border has no full 8-neighbourhood, so it is skipped. */
    for (int y = 1; y < FACE_NORM_H - 1; y++) {
        for (int x = 1; x < FACE_NORM_W - 1; x++) {
            const uint8_t c = gray[y * FACE_NORM_W + x];
            const uint8_t *r0 = &gray[(y - 1) * FACE_NORM_W + x];
            const uint8_t *r1 = &gray[y * FACE_NORM_W + x];
            const uint8_t *r2 = &gray[(y + 1) * FACE_NORM_W + x];

            /* Clockwise from top-left, the conventional bit order. */
            const uint8_t code =
                (uint8_t)(((r0[-1] >= c) << 7) |
                          ((r0[0]  >= c) << 6) |
                          ((r0[1]  >= c) << 5) |
                          ((r1[1]  >= c) << 4) |
                          ((r2[1]  >= c) << 3) |
                          ((r2[0]  >= c) << 2) |
                          ((r2[-1] >= c) << 1) |
                          ((r1[-1] >= c) << 0));

            const int cell = (y / CELL_H) * LBP_GRID + (x / CELL_W);
            counts[cell][s_uniform[code]]++;
        }
    }

    /* Each cell is normalised on its own, so a cell that happens to sit on a
     * shadow does not drag the whole descriptor with it. */
    for (int cell = 0; cell < LBP_GRID * LBP_GRID; cell++) {
        uint32_t total = 0;
        for (int b = 0; b < LBP_BINS; b++) {
            total += counts[cell][b];
        }

        uint8_t *out = desc + cell * LBP_BINS;
        if (total == 0) {
            memset(out, 0, LBP_BINS);
            continue;
        }
        for (int b = 0; b < LBP_BINS; b++) {
            out[b] = (uint8_t)((counts[cell][b] * 255u + total / 2) / total);
        }
    }
}

float face_lbp_distance(const uint8_t *a, const uint8_t *b)
{
    float total = 0.0f;
    int   cells = 0;

    for (int cell = 0; cell < LBP_GRID * LBP_GRID; cell++) {
        const uint8_t *ha = a + cell * LBP_BINS;
        const uint8_t *hb = b + cell * LBP_BINS;

        float sa = 0.0f, sb = 0.0f;
        for (int i = 0; i < LBP_BINS; i++) {
            sa += ha[i];
            sb += hb[i];
        }
        if (sa <= 0.0f || sb <= 0.0f) {
            continue;
        }

        float d = 0.0f;
        for (int i = 0; i < LBP_BINS; i++) {
            const float x = ha[i] / sa;
            const float y = hb[i] / sb;
            const float s = x + y;
            if (s > 0.0f) {
                const float diff = x - y;
                d += (diff * diff) / s;
            }
        }
        total += d;
        cells++;
    }

    return cells ? (total / (float)cells) : 2.0f;
}

uint8_t face_lbp_confidence(float distance)
{
    if (distance <= DIST_CERTAIN) {
        return 100;
    }
    if (distance >= DIST_HOPELESS) {
        return 0;
    }
    const float span = DIST_HOPELESS - DIST_CERTAIN;
    return (uint8_t)(100.0f * (DIST_HOPELESS - distance) / span);
}
