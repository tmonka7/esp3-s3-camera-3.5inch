#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     mounted;
    uint64_t total_bytes;
    uint64_t used_bytes;
    char     name[32];       /* card product name */
    uint32_t speed_khz;
} bsp_storage_info_t;

/** Mounts the TF card at /sdcard over SDMMC 1-bit. */
esp_err_t bsp_storage_mount(void);
esp_err_t bsp_storage_unmount(void);
bool      bsp_storage_mounted(void);

/** Refreshes and returns capacity figures. Cheap enough for a 1 Hz UI timer. */
esp_err_t bsp_storage_info(bsp_storage_info_t *out);

/** Creates /sdcard/<sub> if it does not exist. */
esp_err_t bsp_storage_ensure_dir(const char *path);

/** Mounts the internal `assets` SPIFFS partition at /assets. */
esp_err_t bsp_storage_mount_assets(void);

#ifdef __cplusplus
}
#endif
