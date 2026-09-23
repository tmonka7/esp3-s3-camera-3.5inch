#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "face_lbp.h"
#include "face_locate.h"

static const char *TAG = "face_loc";

/* The segmentation runs on a coarse grid rather than every pixel: a head is
 * an enormous object at this scale, and the flood fill gets ~16x cheaper. */
#define GRID_MAX_W      96
#define GRID_MAX_H      96
#define GRID_CELLS      (GRID_MAX_W * GRID_MAX_H)
#define GRID_TARGET_W   72      /* aim for roughly this many columns */

/* Chai & Ngan's skin band, plus a floor on luma so sensor noise in the dark
 * does not read as a face. */
#define CB_MIN  77
#define CB_MAX  127
#define CR_MIN  133
#define CR_MAX  173
#define Y_MIN   40

/* A head is taller than it is wide, but not by much once hair and neck are
 * in the box. Outside this the region is an arm, a table or a wall. */
#define ASPECT_MIN      0.75f
#define ASPECT_MAX      2.10f
#define MIN_CELLS       24      /* below this the crop has no detail left  */
#define MIN_FILL        0.45f   /* blob must fill its own bounding box     */

static uint8_t  *s_mask;        /* 0 = not skin, 1 = skin, 2+ = label      */
static uint16_t *s_stack;

esp_err_t face_locate_init(void)
{
    if (s_mask) {
        return ESP_OK;
    }

    s_mask  = heap_caps_malloc(GRID_CELLS, MALLOC_CAP_SPIRAM);
    s_stack = heap_caps_malloc(GRID_CELLS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_mask && s_stack, ESP_ERR_NO_MEM, TAG, "grid");

    return ESP_OK;
}

/** Big-endian RGB565, the byte order jpg2rgb565 emits. */
static inline void unpack(const uint8_t *p, int *r, int *g, int *b)
{
    const uint16_t v = (uint16_t)((p[0] << 8) | p[1]);
    *r = ((v >> 11) & 0x1F) << 3;
    *g = ((v >> 5)  & 0x3F) << 2;
    *b = (v & 0x1F) << 3;
}

static inline bool is_skin(int r, int g, int b)
{
    /* Integer BT.601. The shifts keep this off the FPU, which matters when
     * it runs for every cell of every frame. */
    const int y  = (77 * r + 150 * g + 29 * b) >> 8;
    const int cb = 128 + ((-43 * r - 85 * g + 128 * b) >> 8);
    const int cr = 128 + ((128 * r - 107 * g - 21 * b) >> 8);

    return y >= Y_MIN &&
           cb >= CB_MIN && cb <= CB_MAX &&
           cr >= CR_MIN && cr <= CR_MAX;
}

bool face_locate(const uint8_t *rgb, int w, int h,
                 uint16_t *fx, uint16_t *fy, uint16_t *fw, uint16_t *fh)
{
    if (!s_mask || !rgb || w <= 0 || h <= 0) {
        return false;
    }

    /* Pick a step that puts the grid near GRID_TARGET_W and inside the
     * buffer whatever frame size the camera is set to. */
    int step = (w + GRID_TARGET_W - 1) / GRID_TARGET_W;
    if (step < 1) {
        step = 1;
    }
    while (w / step > GRID_MAX_W || h / step > GRID_MAX_H) {
        step++;
    }

    const int gw = w / step;
    const int gh = h / step;
    if (gw < 8 || gh < 8) {
        return false;
    }

    /* ---- segment ---- */
    for (int gy = 0; gy < gh; gy++) {
        const uint8_t *row = rgb + (size_t)(gy * step) * w * 2;
        uint8_t       *out = s_mask + gy * gw;

        for (int gx = 0; gx < gw; gx++) {
            int r, g, b;
            unpack(row + (size_t)(gx * step) * 2, &r, &g, &b);
            out[gx] = is_skin(r, g, b) ? 1 : 0;
        }
    }

    /* ---- largest plausible connected region ---- */
    int best_score = 0;
    int best_x0 = 0, best_y0 = 0, best_x1 = 0, best_y1 = 0;

    for (int start = 0; start < gw * gh; start++) {
        if (s_mask[start] != 1) {
            continue;
        }

        /* Iterative flood fill -- a recursive one would blow the task stack
         * on a frame that is mostly skin-coloured. */
        int sp = 0;
        s_stack[sp++] = (uint16_t)start;
        s_mask[start] = 2;

        int count = 0;
        int x0 = gw, y0 = gh, x1 = -1, y1 = -1;

        while (sp > 0) {
            const int idx = s_stack[--sp];
            const int x   = idx % gw;
            const int y   = idx / gw;

            count++;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;

            if (x > 0      && s_mask[idx - 1]  == 1) { s_mask[idx - 1]  = 2; s_stack[sp++] = (uint16_t)(idx - 1); }
            if (x < gw - 1 && s_mask[idx + 1]  == 1) { s_mask[idx + 1]  = 2; s_stack[sp++] = (uint16_t)(idx + 1); }
            if (y > 0      && s_mask[idx - gw] == 1) { s_mask[idx - gw] = 2; s_stack[sp++] = (uint16_t)(idx - gw); }
            if (y < gh - 1 && s_mask[idx + gw] == 1) { s_mask[idx + gw] = 2; s_stack[sp++] = (uint16_t)(idx + gw); }
        }

        if (count < MIN_CELLS) {
            continue;
        }

        const int   bw     = x1 - x0 + 1;
        const int   bh     = y1 - y0 + 1;
        const float aspect = (float)bh / (float)bw;
        const float fill   = (float)count / (float)(bw * bh);

        if (aspect < ASPECT_MIN || aspect > ASPECT_MAX || fill < MIN_FILL) {
            continue;
        }

        /* Among the shapes that could be a head, the biggest wins. A hand is
         * the usual runner-up and is reliably smaller than a face at the
         * distance someone stands to be let in. */
        if (count > best_score) {
            best_score = count;
            best_x0 = x0; best_y0 = y0; best_x1 = x1; best_y1 = y1;
        }
    }

    if (best_score == 0) {
        return false;
    }

    int px0 = best_x0 * step;
    int py0 = best_y0 * step;
    int px1 = (best_x1 + 1) * step;
    int py1 = (best_y1 + 1) * step;

    /* Skin segmentation stops at the hairline and the chin, so the box is
     * consistently tighter than the face. Growing it a little puts the eyes
     * and mouth where the descriptor expects them. */
    const int grow_x = (px1 - px0) / 8;
    const int grow_y = (py1 - py0) / 6;

    px0 -= grow_x; px1 += grow_x;
    py0 -= grow_y; py1 += grow_y;

    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > w) px1 = w;
    if (py1 > h) py1 = h;

    *fx = (uint16_t)px0;
    *fy = (uint16_t)py0;
    *fw = (uint16_t)(px1 - px0);
    *fh = (uint16_t)(py1 - py0);
    return (*fw >= 16 && *fh >= 16);
}

void face_crop_gray(const uint8_t *rgb, int w, int h,
                    uint16_t fx, uint16_t fy, uint16_t fw, uint16_t fh,
                    uint8_t *out_gray)
{
    if (!rgb || !out_gray || fw == 0 || fh == 0) {
        return;
    }

    for (int oy = 0; oy < FACE_NORM_H; oy++) {
        int sy = fy + (oy * fh) / FACE_NORM_H;
        if (sy >= h) {
            sy = h - 1;
        }
        const uint8_t *row = rgb + (size_t)sy * w * 2;

        for (int ox = 0; ox < FACE_NORM_W; ox++) {
            int sx = fx + (ox * fw) / FACE_NORM_W;
            if (sx >= w) {
                sx = w - 1;
            }

            int r, g, b;
            unpack(row + (size_t)sx * 2, &r, &g, &b);
            out_gray[oy * FACE_NORM_W + ox] =
                (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
        }
    }
}
