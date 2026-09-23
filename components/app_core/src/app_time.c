#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_check.h"

#include "bsp_board.h"
#include "bsp_i2c.h"

#include "app_events.h"
#include "app_settings.h"
#include "app_time.h"

static const char *TAG = "app_time";

/* ---- PCF85063 ---------------------------------------------------------- */
#define PCF_REG_CTRL1    0x00
#define PCF_REG_SECONDS  0x04    /* bit 7 = oscillator-stop flag */

static bool s_rtc_present;
static bool s_time_valid;

static inline uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t rtc_read(struct tm *out)
{
    uint8_t reg = PCF_REG_SECONDS;
    uint8_t r[7] = {0};
    ESP_RETURN_ON_ERROR(bsp_i2c_write_read(BSP_I2C_ADDR_PCF85063, &reg, 1, r, sizeof(r)),
                        TAG, "rtc read");

    if (r[0] & 0x80) {           /* oscillator stopped -> contents are garbage */
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd2dec(r[0] & 0x7F);
    out->tm_min  = bcd2dec(r[1] & 0x7F);
    out->tm_hour = bcd2dec(r[2] & 0x3F);
    out->tm_mday = bcd2dec(r[3] & 0x3F);
    out->tm_wday = r[4] & 0x07;
    out->tm_mon  = bcd2dec(r[5] & 0x1F) - 1;
    out->tm_year = bcd2dec(r[6]) + 100;   /* PCF85063 counts 2000..2099 */
    return ESP_OK;
}

static esp_err_t rtc_write(const struct tm *t)
{
    const uint8_t buf[8] = {
        PCF_REG_SECONDS,
        dec2bcd((uint8_t)t->tm_sec) & 0x7F,   /* clearing bit 7 restarts the osc */
        dec2bcd((uint8_t)t->tm_min),
        dec2bcd((uint8_t)t->tm_hour),
        dec2bcd((uint8_t)t->tm_mday),
        (uint8_t)(t->tm_wday & 0x07),
        dec2bcd((uint8_t)(t->tm_mon + 1)),
        dec2bcd((uint8_t)(t->tm_year % 100)),
    };
    return bsp_i2c_write(BSP_I2C_ADDR_PCF85063, buf, sizeof(buf));
}

/* ---- Offline / manual-only clock ------------------------------------------------ */
void app_time_apply_timezone(void)
{
    const char *tz = app_settings()->timezone;
    setenv("TZ", (tz && tz[0]) ? tz : "UTC0", 1);
    tzset();
}

esp_err_t app_time_init(void)
{
    app_time_apply_timezone();

    s_rtc_present = bsp_i2c_probe(BSP_I2C_ADDR_PCF85063);
    if (s_rtc_present) {
        struct tm utc;
        if (rtc_read(&utc) == ESP_OK && utc.tm_year >= 120) {   /* >= year 2020 */
            const time_t     secs = time(&utc);   /* the RTC holds UTC */
            const struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            s_time_valid = true;
            ESP_LOGI(TAG, "clock restored from RTC");
        } else {
            ESP_LOGW(TAG, "RTC has no valid time yet");
        }
    } else {
        ESP_LOGW(TAG, "no PCF85063 found -- time survives only while powered");
    }

    /* Manual clock entry is the intended operating mode for this device.
     * Network-based SNTP is intentionally disabled so the product remains
     * fully usable in offline or isolated deployments. */
    ESP_LOGI(TAG, "time sync is manual-only; SNTP disabled for offline operation");
    return ESP_OK;
}

bool app_time_is_valid(void)
{
    return s_time_valid;
}

esp_err_t app_time_set(const struct tm *tm_utc)
{
    ESP_RETURN_ON_FALSE(tm_utc, ESP_ERR_INVALID_ARG, TAG, "null");

    struct tm copy = *tm_utc;
    copy.tm_isdst = 0;

    /* `timegm()` is not available on all libc builds; `mktime()` on a UTC-
     * normalized `struct tm` gives the same result for the RTC value we want. */
    const time_t secs = mktime(&copy);
    if (secs == (time_t)-1) {
        return ESP_ERR_INVALID_ARG;
    }
    const struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_time_valid = true;

    if (s_rtc_present) {
        ESP_RETURN_ON_ERROR(rtc_write(&copy), TAG, "rtc write");
    }
    app_event_post(APP_EVT_TIME_SYNC, NULL, 0);
    return ESP_OK;
}

void app_time_format(char *out, size_t len, const char *fmt)
{
    if (!out || len == 0) {
        return;
    }
    const time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(out, len, fmt, &local);
}

void app_time_stamp_filename(char *out, size_t len)
{
    app_time_format(out, len, "%Y-%m-%d_%H-%M-%S");
}
