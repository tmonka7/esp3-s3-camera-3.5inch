#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_check.h"

#include "bsp_board.h"
#include "bsp_storage.h"
#include "media_files.h"

static const char *TAG = "media_files";

static const char *dir_for(media_kind_t kind)
{
    switch (kind) {
    case MEDIA_KIND_VIDEO: return BSP_SD_MOUNT_POINT "/video";
    case MEDIA_KIND_IMAGE: return BSP_SD_MOUNT_POINT "/image";
    case MEDIA_KIND_LOG:   return BSP_SD_MOUNT_POINT "/logs";
    default:               return NULL;
    }
}

static media_kind_t kind_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return MEDIA_KIND_ANY;
    }
    if (strcasecmp(dot, ".avi") == 0) return MEDIA_KIND_VIDEO;
    if (strcasecmp(dot, ".jpg") == 0) return MEDIA_KIND_IMAGE;
    if (strcasecmp(dot, ".log") == 0) return MEDIA_KIND_LOG;
    return MEDIA_KIND_ANY;
}

static int by_mtime_desc(const void *a, const void *b)
{
    const media_entry_t *ea = a;
    const media_entry_t *eb = b;

    if (ea->mtime != eb->mtime) {
        return (eb->mtime > ea->mtime) ? 1 : -1;
    }
    /* File names are timestamps, so they break ties in the same direction
     * even when the card's clock resolution collapses two mtimes. */
    return strcmp(eb->name, ea->name);
}

static size_t scan_dir(const char *dir, media_kind_t want,
                       media_entry_t *out, size_t max, size_t used)
{
    DIR *d = opendir(dir);
    if (!d) {
        return used;
    }

    struct dirent *e;
    while (used < max && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }

        const media_kind_t kind = kind_of(e->d_name);
        if (want != MEDIA_KIND_ANY && kind != want) {
            continue;
        }

        char path[256];
        size_t used_dir = strlcpy(path, dir, sizeof(path));
        if (used_dir >= sizeof(path)) {
            continue;
        }
        if (strlcat(path, "/", sizeof(path)) >= sizeof(path)) {
            continue;
        }
        if (strlcat(path, e->d_name, sizeof(path)) >= sizeof(path)) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        media_entry_t *slot = &out[used++];
        strlcpy(slot->name, e->d_name, MEDIA_NAME_LEN);
        slot->size  = (uint32_t)st.st_size;
        slot->mtime = st.st_mtime;
        slot->kind  = kind;
    }

    closedir(d);
    return used;
}

size_t media_files_list(media_kind_t kind, media_entry_t *out, size_t max)
{
    if (!out || max == 0 || !bsp_storage_mounted()) {
        return 0;
    }

    size_t count = 0;
    if (kind == MEDIA_KIND_ANY) {
        count = scan_dir(dir_for(MEDIA_KIND_VIDEO), MEDIA_KIND_VIDEO, out, max, count);
        count = scan_dir(dir_for(MEDIA_KIND_IMAGE), MEDIA_KIND_IMAGE, out, max, count);
        count = scan_dir(dir_for(MEDIA_KIND_LOG),   MEDIA_KIND_LOG,   out, max, count);
    } else {
        count = scan_dir(dir_for(kind), kind, out, max, count);
    }

    qsort(out, count, sizeof(media_entry_t), by_mtime_desc);
    return count;
}

/** Recordings are named "YYYY-MM-DD_HH-MM-SS.ext"; the date is the prefix. */
static bool date_of(const media_entry_t *e, char *out, size_t out_len)
{
    if (strlen(e->name) < 10 || e->name[4] != '-' || e->name[7] != '-') {
        return false;
    }
    if (out_len < 11) {
        return false;
    }
    memcpy(out, e->name, 10);
    out[10] = '\0';
    return true;
}

size_t media_files_group_by_day(const media_entry_t *entries, size_t count,
                                media_day_t *out, size_t max)
{
    if (!entries || !out || max == 0) {
        return 0;
    }

    size_t groups = 0;
    for (size_t i = 0; i < count; i++) {
        char date[12];
        if (!date_of(&entries[i], date, sizeof(date))) {
            continue;
        }

        /* The list is already sorted newest-first, so a day's files are
         * contiguous and only the most recent group needs checking. */
        if (groups > 0 && strcmp(out[groups - 1].date, date) == 0 &&
            out[groups - 1].kind == entries[i].kind) {
            out[groups - 1].count++;
            out[groups - 1].bytes += entries[i].size;
            continue;
        }

        if (groups == max) {
            break;
        }
        strlcpy(out[groups].date, date, sizeof(out[groups].date));
        out[groups].count = 1;
        out[groups].bytes = entries[i].size;
        out[groups].kind  = entries[i].kind;
        groups++;
    }
    return groups;
}

void media_files_path(const media_entry_t *e, char *out, size_t out_len)
{
    if (!e || !out || out_len == 0) {
        return;
    }

    const char *dir = dir_for(e->kind);
    if (!dir) {
        out[0] = '\0';
        return;
    }

    size_t used = strlcpy(out, dir, out_len);
    if (used >= out_len) {
        out[out_len - 1] = '\0';
        return;
    }
    if (strlcat(out, "/", out_len) >= out_len) {
        out[out_len - 1] = '\0';
        return;
    }
    if (strlcat(out, e->name, out_len) >= out_len) {
        out[out_len - 1] = '\0';
    }
}

esp_err_t media_files_delete(const media_entry_t *e)
{
    ESP_RETURN_ON_FALSE(e, ESP_ERR_INVALID_ARG, TAG, "null");

    char path[192];
    media_files_path(e, path, sizeof(path));

    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "cannot delete %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted %s", path);
    return ESP_OK;
}

esp_err_t media_files_delete_day(const char *date, media_kind_t kind)
{
    ESP_RETURN_ON_FALSE(date, ESP_ERR_INVALID_ARG, TAG, "null");

    media_entry_t *entries = calloc(MEDIA_MAX_ENTRIES, sizeof(media_entry_t));
    ESP_RETURN_ON_FALSE(entries, ESP_ERR_NO_MEM, TAG, "alloc");

    const size_t count = media_files_list(kind, entries, MEDIA_MAX_ENTRIES);
    size_t       gone  = 0;

    for (size_t i = 0; i < count; i++) {
        char d[12];
        if (date_of(&entries[i], d, sizeof(d)) && strcmp(d, date) == 0) {
            if (media_files_delete(&entries[i]) == ESP_OK) {
                gone++;
            }
        }
    }

    free(entries);
    ESP_LOGI(TAG, "deleted %u files from %s", (unsigned)gone, date);
    return gone > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void media_files_format_size(uint32_t bytes, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    if (bytes >= 1024u * 1024u) {
        /* One decimal without floating point: 5242880 -> "5.0 MB". */
        const uint32_t mb    = bytes / (1024u * 1024u);
        const uint32_t tenth = (bytes % (1024u * 1024u)) * 10u / (1024u * 1024u);
        snprintf(out, out_len, "%lu.%lu MB", (unsigned long)mb, (unsigned long)tenth);
    } else if (bytes >= 1024u) {
        snprintf(out, out_len, "%lu KB", (unsigned long)(bytes / 1024u));
    } else {
        snprintf(out, out_len, "%lu B", (unsigned long)bytes);
    }
}
