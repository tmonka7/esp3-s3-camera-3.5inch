#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "bsp_board.h"
#include "bsp_storage.h"

static const char *TAG = "bsp_storage";

static sdmmc_card_t *s_card;
static bool          s_mounted;
static bool          s_assets_mounted;

esp_err_t bsp_storage_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* The card shares the board with a 40 MHz LCD bus and the DVP camera;
     * 20 MHz in 1-bit mode is the reliable operating point here. */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk   = BSP_SD_PIN_CLK;
    slot.cmd   = BSP_SD_PIN_CMD;
    slot.d0    = BSP_SD_PIN_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = CONFIG_BSP_SD_FORMAT_IF_MOUNT_FAILED,
        .max_files              = 8,
        .allocation_unit_size   = 32 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TF card mount failed: %s", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "TF card mounted: %s, %lluMB",
             s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);

    /* The layout every media/log path below depends on. */
    bsp_storage_ensure_dir(BSP_SD_MOUNT_POINT "/video");
    bsp_storage_ensure_dir(BSP_SD_MOUNT_POINT "/image");
    bsp_storage_ensure_dir(BSP_SD_MOUNT_POINT "/events");
    bsp_storage_ensure_dir(BSP_SD_MOUNT_POINT "/logs");
    return ESP_OK;
}

esp_err_t bsp_storage_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
    s_card    = NULL;
    s_mounted = false;
    return err;
}

bool bsp_storage_mounted(void)
{
    return s_mounted;
}

esp_err_t bsp_storage_info(bsp_storage_info_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "null");

    memset(out, 0, sizeof(*out));
    if (!s_mounted || !s_card) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0, freeb = 0;
    ESP_RETURN_ON_ERROR(esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeb),
                        TAG, "fat info");

    out->mounted     = true;
    out->total_bytes = total;
    out->used_bytes  = total - freeb;
    out->speed_khz   = s_card->max_freq_khz;
    strlcpy(out->name, s_card->cid.name, sizeof(out->name));
    return ESP_OK;
}

esp_err_t bsp_storage_ensure_dir(const char *path)
{
    ESP_RETURN_ON_FALSE(path, ESP_ERR_INVALID_ARG, TAG, "null");

    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: %d", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t bsp_storage_mount_assets(void)
{
    if (s_assets_mounted) {
        return ESP_OK;
    }

    const esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/assets",
        .partition_label        = "assets",
        .max_files              = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "assets partition mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_assets_mounted = true;
    return ESP_OK;
}
