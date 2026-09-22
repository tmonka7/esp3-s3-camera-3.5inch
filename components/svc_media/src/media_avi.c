#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "media_avi.h"

static const char *TAG = "avi";

/* Byte offsets into the fixed-size header written by write_header(). They are
 * patched at close once the real totals are known. */
#define OFF_RIFF_SIZE        4
#define OFF_AVIH_US_FRAME    32
#define OFF_AVIH_MAX_RATE    36
#define OFF_AVIH_TOTAL_FRM   48
#define OFF_STRH_RATE       132
#define OFF_STRH_LENGTH     140
#define OFF_MOVI_SIZE       216
#define HEADER_SIZE         224      /* first byte of the first frame chunk */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t patch_u32(FILE *fp, long offset, uint32_t value)
{
    uint8_t buf[4];
    put_u32(buf, value);
    if (fseek(fp, offset, SEEK_SET) != 0 || fwrite(buf, 1, 4, fp) != 4) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Writer
 * ------------------------------------------------------------------------ */
static esp_err_t write_header(avi_writer_t *w)
{
    uint8_t h[HEADER_SIZE];
    memset(h, 0, sizeof(h));

    size_t o = 0;
    memcpy(h + o, "RIFF", 4);           o += 4;
    put_u32(h + o, 0);                  o += 4;   /* patched: file size - 8 */
    memcpy(h + o, "AVI ", 4);           o += 4;

    /* LIST hdrl -- fixed length for this single-stream layout. */
    memcpy(h + o, "LIST", 4);           o += 4;
    put_u32(h + o, 192);                o += 4;
    memcpy(h + o, "hdrl", 4);           o += 4;

    /* avih */
    memcpy(h + o, "avih", 4);           o += 4;
    put_u32(h + o, 56);                 o += 4;
    put_u32(h + o, 33333);              o += 4;   /* us/frame, patched  */
    put_u32(h + o, 0);                  o += 4;   /* max byte rate, patched */
    put_u32(h + o, 0);                  o += 4;   /* padding granularity */
    put_u32(h + o, 0x10);               o += 4;   /* AVIF_HASINDEX      */
    put_u32(h + o, 0);                  o += 4;   /* total frames, patched */
    put_u32(h + o, 0);                  o += 4;   /* initial frames     */
    put_u32(h + o, 1);                  o += 4;   /* streams            */
    put_u32(h + o, 0);                  o += 4;   /* suggested buffer   */
    put_u32(h + o, w->width);           o += 4;
    put_u32(h + o, w->height);          o += 4;
    memset(h + o, 0, 16);               o += 16;  /* reserved           */

    /* LIST strl */
    memcpy(h + o, "LIST", 4);           o += 4;
    put_u32(h + o, 116);                o += 4;
    memcpy(h + o, "strl", 4);           o += 4;

    /* strh */
    memcpy(h + o, "strh", 4);           o += 4;
    put_u32(h + o, 56);                 o += 4;
    memcpy(h + o, "vids", 4);           o += 4;
    memcpy(h + o, "MJPG", 4);           o += 4;
    put_u32(h + o, 0);                  o += 4;   /* flags              */
    put_u32(h + o, 0);                  o += 4;   /* priority+language  */
    put_u32(h + o, 0);                  o += 4;   /* initial frames     */
    put_u32(h + o, 1);                  o += 4;   /* scale              */
    put_u32(h + o, 30);                 o += 4;   /* rate, patched      */
    put_u32(h + o, 0);                  o += 4;   /* start              */
    put_u32(h + o, 0);                  o += 4;   /* length, patched    */
    put_u32(h + o, 0);                  o += 4;   /* suggested buffer   */
    put_u32(h + o, 0xFFFFFFFF);         o += 4;   /* quality            */
    put_u32(h + o, 0);                  o += 4;   /* sample size        */
    put_u16(h + o, 0);                  o += 2;   /* rcFrame left       */
    put_u16(h + o, 0);                  o += 2;
    put_u16(h + o, w->width);           o += 2;
    put_u16(h + o, w->height);          o += 2;

    /* strf -- BITMAPINFOHEADER */
    memcpy(h + o, "strf", 4);           o += 4;
    put_u32(h + o, 40);                 o += 4;
    put_u32(h + o, 40);                 o += 4;   /* biSize             */
    put_u32(h + o, w->width);           o += 4;
    put_u32(h + o, w->height);          o += 4;
    put_u16(h + o, 1);                  o += 2;   /* planes             */
    put_u16(h + o, 24);                 o += 2;   /* bit count          */
    memcpy(h + o, "MJPG", 4);           o += 4;
    put_u32(h + o, (uint32_t)w->width * w->height * 3); o += 4;
    put_u32(h + o, 0);                  o += 4;
    put_u32(h + o, 0);                  o += 4;
    put_u32(h + o, 0);                  o += 4;
    put_u32(h + o, 0);                  o += 4;

    /* LIST movi */
    memcpy(h + o, "LIST", 4);           o += 4;
    put_u32(h + o, 0);                  o += 4;   /* patched            */
    memcpy(h + o, "movi", 4);           o += 4;

    if (o != HEADER_SIZE) {
        /* A layout change that desynchronises the patch offsets would corrupt
         * every file; fail loudly instead. */
        ESP_LOGE(TAG, "header is %u bytes, expected %d", (unsigned)o, HEADER_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    return fwrite(h, 1, HEADER_SIZE, w->fp) == HEADER_SIZE ? ESP_OK : ESP_FAIL;
}

esp_err_t avi_writer_open(avi_writer_t *w, const char *path,
                          uint16_t width, uint16_t height)
{
    ESP_RETURN_ON_FALSE(w && path, ESP_ERR_INVALID_ARG, TAG, "null");

    memset(w, 0, sizeof(*w));
    w->width  = width;
    w->height = height;
    strlcpy(w->path, path, sizeof(w->path));

    w->index = heap_caps_malloc(AVI_MAX_FRAMES * 2 * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(w->index, ESP_ERR_NO_MEM, TAG, "index alloc");

    w->fp = fopen(path, "wb");
    if (!w->fp) {
        heap_caps_free(w->index);
        w->index = NULL;
        ESP_LOGE(TAG, "cannot create %s", path);
        return ESP_FAIL;
    }

    const esp_err_t err = write_header(w);
    if (err != ESP_OK) {
        fclose(w->fp);
        heap_caps_free(w->index);
        memset(w, 0, sizeof(*w));
        return err;
    }

    w->start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    w->open     = true;
    ESP_LOGI(TAG, "recording to %s (%ux%u)", path, width, height);
    return ESP_OK;
}

esp_err_t avi_writer_add_frame(avi_writer_t *w, const uint8_t *jpeg, size_t len)
{
    ESP_RETURN_ON_FALSE(w && w->open, ESP_ERR_INVALID_STATE, TAG, "closed");
    ESP_RETURN_ON_FALSE(jpeg && len, ESP_ERR_INVALID_ARG, TAG, "empty frame");

    if (w->frame_count >= AVI_MAX_FRAMES) {
        return ESP_ERR_NO_MEM;
    }

    const long chunk_off = ftell(w->fp);
    if (chunk_off < 0) {
        return ESP_FAIL;
    }

    uint8_t hdr[8];
    memcpy(hdr, "00dc", 4);
    put_u32(hdr + 4, (uint32_t)len);
    if (fwrite(hdr, 1, 8, w->fp) != 8 || fwrite(jpeg, 1, len, w->fp) != len) {
        ESP_LOGE(TAG, "write failed -- card full or removed");
        return ESP_FAIL;
    }

    /* RIFF chunks are word-aligned. */
    if (len & 1) {
        const uint8_t pad = 0;
        fwrite(&pad, 1, 1, w->fp);
    }

    /* idx1 offsets are relative to the start of the movi LIST's data. */
    w->index[w->frame_count * 2]     = (uint32_t)(chunk_off - (HEADER_SIZE - 4));
    w->index[w->frame_count * 2 + 1] = (uint32_t)len;

    w->frame_count++;
    w->total_bytes += (uint32_t)len;
    if (len > w->max_frame_bytes) {
        w->max_frame_bytes = (uint32_t)len;
    }
    return ESP_OK;
}

uint32_t avi_writer_elapsed_s(const avi_writer_t *w)
{
    if (!w || !w->open) {
        return 0;
    }
    return ((uint32_t)(esp_timer_get_time() / 1000) - w->start_ms) / 1000;
}

esp_err_t avi_writer_close(avi_writer_t *w)
{
    ESP_RETURN_ON_FALSE(w && w->open, ESP_ERR_INVALID_STATE, TAG, "closed");

    const uint32_t elapsed_ms = (uint32_t)(esp_timer_get_time() / 1000) - w->start_ms;
    const long     movi_end   = ftell(w->fp);

    /* idx1 */
    uint8_t idx_hdr[8];
    memcpy(idx_hdr, "idx1", 4);
    put_u32(idx_hdr + 4, w->frame_count * 16);
    fwrite(idx_hdr, 1, 8, w->fp);

    for (uint32_t i = 0; i < w->frame_count; i++) {
        uint8_t e[16];
        memcpy(e, "00dc", 4);
        put_u32(e + 4,  0x10);                        /* AVIIF_KEYFRAME */
        put_u32(e + 8,  w->index[i * 2]);
        put_u32(e + 12, w->index[i * 2 + 1]);
        fwrite(e, 1, 16, w->fp);
    }

    const long file_end = ftell(w->fp);

    /* Derive the real frame rate from wall-clock time: the sensor rarely
     * delivers exactly the nominal rate once the card is busy. */
    uint32_t fps = 25;
    if (elapsed_ms > 0 && w->frame_count > 1) {
        fps = (w->frame_count * 1000u) / elapsed_ms;
        if (fps == 0)  fps = 1;
        if (fps > 120) fps = 120;
    }
    const uint32_t us_per_frame = 1000000u / fps;
    const uint32_t byte_rate    = (elapsed_ms > 0)
                                ? (uint32_t)(((uint64_t)w->total_bytes * 1000u) / elapsed_ms)
                                : 0;

    esp_err_t err = ESP_OK;
    err |= patch_u32(w->fp, OFF_RIFF_SIZE,      (uint32_t)(file_end - 8));
    err |= patch_u32(w->fp, OFF_AVIH_US_FRAME,  us_per_frame);
    err |= patch_u32(w->fp, OFF_AVIH_MAX_RATE,  byte_rate);
    err |= patch_u32(w->fp, OFF_AVIH_TOTAL_FRM, w->frame_count);
    err |= patch_u32(w->fp, OFF_STRH_RATE,      fps);
    err |= patch_u32(w->fp, OFF_STRH_LENGTH,    w->frame_count);
    err |= patch_u32(w->fp, OFF_MOVI_SIZE,      (uint32_t)(movi_end - (HEADER_SIZE - 4)));

    fclose(w->fp);
    heap_caps_free(w->index);

    ESP_LOGI(TAG, "closed %s: %lu frames, %lu KB, %lu fps",
             w->path, (unsigned long)w->frame_count,
             (unsigned long)(w->total_bytes / 1024), (unsigned long)fps);

    w->fp    = NULL;
    w->index = NULL;
    w->open  = false;
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

/* --------------------------------------------------------------------------
 * Reader
 * ------------------------------------------------------------------------ */
esp_err_t avi_reader_open(avi_reader_t *r, const char *path)
{
    ESP_RETURN_ON_FALSE(r && path, ESP_ERR_INVALID_ARG, TAG, "null");

    memset(r, 0, sizeof(*r));
    r->fp = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(r->fp, ESP_ERR_NOT_FOUND, TAG, "cannot open %s", path);

    uint8_t hdr[HEADER_SIZE];
    if (fread(hdr, 1, sizeof(hdr), r->fp) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "AVI ", 4) != 0) {
        ESP_LOGE(TAG, "%s is not an AVI this writer produced", path);
        fclose(r->fp);
        r->fp = NULL;
        return ESP_ERR_INVALID_ARG;
    }

    r->us_per_frame = get_u32(hdr + OFF_AVIH_US_FRAME);
    r->frame_count  = get_u32(hdr + OFF_AVIH_TOTAL_FRM);
    r->width        = (uint16_t)get_u32(hdr + 64);   /* avih width  */
    r->height       = (uint16_t)get_u32(hdr + 68);   /* avih height */

    if (r->us_per_frame == 0) {
        r->us_per_frame = 40000;   /* 25 fps */
    }

    /* Walk the movi chunks to build the index. Trusting idx1 would be faster,
     * but a recording cut short by a power loss never got one -- scanning
     * makes those files playable too. */
    r->index = heap_caps_malloc(AVI_MAX_FRAMES * 2 * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!r->index) {
        fclose(r->fp);
        r->fp = NULL;
        return ESP_ERR_NO_MEM;
    }

    long pos = HEADER_SIZE;
    while (r->indexed < AVI_MAX_FRAMES) {
        uint8_t ch[8];
        if (fseek(r->fp, pos, SEEK_SET) != 0 || fread(ch, 1, 8, r->fp) != 8) {
            break;
        }
        if (memcmp(ch, "idx1", 4) == 0) {
            break;
        }
        if (memcmp(ch, "00dc", 4) != 0) {
            break;      /* unexpected chunk -- stop rather than guess */
        }

        const uint32_t len = get_u32(ch + 4);
        if (len == 0 || len > 4u * 1024 * 1024) {
            break;
        }

        r->index[r->indexed * 2]     = (uint32_t)(pos + 8);
        r->index[r->indexed * 2 + 1] = len;
        r->indexed++;

        pos += 8 + len + (len & 1);
    }

    if (r->indexed == 0) {
        ESP_LOGE(TAG, "%s contains no frames", path);
        heap_caps_free(r->index);
        fclose(r->fp);
        memset(r, 0, sizeof(*r));
        return ESP_ERR_INVALID_SIZE;
    }

    /* The scan is authoritative: a truncated file's header count is stale. */
    r->frame_count = r->indexed;
    r->open        = true;

    ESP_LOGI(TAG, "%s: %lu frames, %ux%u, %lu us/frame", path,
             (unsigned long)r->frame_count, r->width, r->height,
             (unsigned long)r->us_per_frame);
    return ESP_OK;
}

uint32_t avi_reader_max_frame_bytes(const avi_reader_t *r)
{
    if (!r || !r->open) {
        return 0;
    }
    uint32_t max = 0;
    for (uint32_t i = 0; i < r->indexed; i++) {
        if (r->index[i * 2 + 1] > max) {
            max = r->index[i * 2 + 1];
        }
    }
    return max;
}

esp_err_t avi_reader_frame(avi_reader_t *r, uint32_t n,
                           uint8_t *buf, size_t buf_len, size_t *out_len)
{
    ESP_RETURN_ON_FALSE(r && r->open, ESP_ERR_INVALID_STATE, TAG, "closed");
    ESP_RETURN_ON_FALSE(n < r->frame_count, ESP_ERR_INVALID_ARG, TAG, "range");

    const uint32_t off = r->index[n * 2];
    const uint32_t len = r->index[n * 2 + 1];

    if (out_len) {
        *out_len = len;
    }
    if (!buf || buf_len < len) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (fseek(r->fp, (long)off, SEEK_SET) != 0 || fread(buf, 1, len, r->fp) != len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void avi_reader_close(avi_reader_t *r)
{
    if (!r || !r->open) {
        return;
    }
    fclose(r->fp);
    heap_caps_free(r->index);
    memset(r, 0, sizeof(*r));
}
