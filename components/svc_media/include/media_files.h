/* Directory listing for the Storage and Playback pages.
 *
 * The UI groups recordings by the date in their file name, which is how the
 * mockup presents them ("2025/06/29 -- Video, 12 items").
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_MAX_ENTRIES   256
#define MEDIA_NAME_LEN      64

typedef enum {
    MEDIA_KIND_ANY = 0,
    MEDIA_KIND_VIDEO,      /* .avi */
    MEDIA_KIND_IMAGE,      /* .jpg */
    MEDIA_KIND_LOG,        /* .log */
} media_kind_t;

typedef struct {
    char         name[MEDIA_NAME_LEN];
    uint32_t     size;
    time_t       mtime;
    media_kind_t kind;
} media_entry_t;

/** One calendar day's worth of recordings. */
typedef struct {
    char     date[12];       /* "2026-09-22" */
    uint16_t count;
    uint64_t bytes;
    media_kind_t kind;
} media_day_t;

/**
 * Lists `kind` newest-first into `out`. Returns the number written.
 * The caller supplies the array, so nothing is allocated here.
 */
size_t media_files_list(media_kind_t kind, media_entry_t *out, size_t max);

/** Collapses a listing into per-day groups, newest day first. */
size_t media_files_group_by_day(const media_entry_t *entries, size_t count,
                                media_day_t *out, size_t max);

/** Full path for an entry, e.g. "/sdcard/video/<name>". */
void media_files_path(const media_entry_t *e, char *out, size_t out_len);

esp_err_t media_files_delete(const media_entry_t *e);

/** Deletes every file belonging to one day group. */
esp_err_t media_files_delete_day(const char *date, media_kind_t kind);

/** Human-readable size: "5.2 MB", "812 KB". */
void media_files_format_size(uint32_t bytes, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
