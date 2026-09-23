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

/* --------------------------------------------------------------------------
 * Calendar maths
 *
 * Converting a UTC struct tm to an epoch has no portable libc answer here:
 * timegm() is gated behind newlib feature macros, and mktime() interprets its
 * argument as LOCAL time, so it is off by the UTC offset the moment a real
 * timezone is configured. This is the standard days-from-civil algorithm and
 * is exact for every date the RTC can hold.
 * ------------------------------------------------------------------------ */
static time_t tm_to_utc(const struct tm *t)
{
    int            y = t->tm_year + 1900;
    const unsigned m = (unsigned)t->tm_mon + 1;
    const unsigned d = (unsigned)t->tm_mday;

    y -= (m <= 2);                                  /* March-based year */
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              /* 0..399   */
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;

    /* 719468 shifts the 0000-03-01 epoch of the algorithm to 1970-01-01. */
    const long long days = (long long)era * 146097LL + (long long)doe - 719468LL;

    return (time_t)(days * 86400LL +
                    (long long)t->tm_hour * 3600LL +
                    (long long)t->tm_min * 60LL +
                    (long long)t->tm_sec);
}

int app_time_days_in_month(int year, int month)
{
    static const int k_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month < 1 || month > 12) {
        return 31;
    }
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return k_days[month - 1];
}

/* --------------------------------------------------------------------------
 * Clock
 * ------------------------------------------------------------------------ */
void app_time_apply_timezone(void)
{
    const char *tz = app_settings()->timezone;
    setenv("TZ", (tz && tz[0]) ? tz : "UTC0", 1);
    tzset();
}

esp_err_t app_time_init(void)
{
    /* The timezone has to be in place before any conversion below. */
    app_time_apply_timezone();

    s_rtc_present = bsp_i2c_probe(BSP_I2C_ADDR_PCF85063);
    if (!s_rtc_present) {
        ESP_LOGW(TAG, "no PCF85063 found -- set the clock from Settings after "
                      "every power cut");
        return ESP_OK;
    }

    struct tm utc;
    if (rtc_read(&utc) == ESP_OK && utc.tm_year >= 120) {   /* >= year 2020 */
        const struct timeval tv = { .tv_sec = tm_to_utc(&utc), .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_time_valid = true;

        char now[32];
        app_time_format(now, sizeof(now), "%Y-%m-%d %H:%M:%S");
        ESP_LOGI(TAG, "clock restored from RTC: %s local", now);
    } else {
        ESP_LOGW(TAG, "RTC holds no valid time -- set it from Settings");
    }

    return ESP_OK;
}

bool app_time_is_valid(void)
{
    return s_time_valid;
}

bool app_time_has_rtc(void)
{
    return s_rtc_present;
}

/** Commits an epoch to the system clock and, when fitted, to the RTC. */
static esp_err_t commit_epoch(time_t secs)
{
    const struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }
    s_time_valid = true;

    if (s_rtc_present) {
        struct tm utc;
        gmtime_r(&secs, &utc);
        ESP_RETURN_ON_ERROR(rtc_write(&utc), TAG, "rtc write");
    }

    app_event_post(APP_EVT_TIME_SYNC, NULL, 0);
    return ESP_OK;
}

esp_err_t app_time_set_local(const struct tm *local_tm)
{
    ESP_RETURN_ON_FALSE(local_tm, ESP_ERR_INVALID_ARG, TAG, "null");

    struct tm copy = *local_tm;
    /* Let the C library work out whether DST applies to this local time. */
    copy.tm_isdst = -1;

    /* mktime() is the right tool here and only here: its argument really is
     * local time, which is exactly what the user typed. */
    const time_t secs = mktime(&copy);
    ESP_RETURN_ON_FALSE(secs != (time_t)-1, ESP_ERR_INVALID_ARG, TAG, "bad date");

    ESP_RETURN_ON_ERROR(commit_epoch(secs), TAG, "commit");

    char now[32];
    app_time_format(now, sizeof(now), "%Y-%m-%d %H:%M:%S");
    ESP_LOGI(TAG, "clock set by hand to %s local", now);
    return ESP_OK;
}

esp_err_t app_time_set_utc(const struct tm *utc_tm)
{
    ESP_RETURN_ON_FALSE(utc_tm, ESP_ERR_INVALID_ARG, TAG, "null");
    return commit_epoch(tm_to_utc(utc_tm));
}

void app_time_get_local(struct tm *out)
{
    if (!out) {
        return;
    }
    const time_t now = time(NULL);
    localtime_r(&now, out);
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
