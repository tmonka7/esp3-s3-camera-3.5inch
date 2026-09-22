#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "bsp_board.h"
#include "bsp_i2c.h"
#include "bsp_camera.h"
#include "bsp_io_expander.h"

static const char *TAG = "bsp_cam";

static bool        s_ready;
static framesize_t s_framesize = FRAMESIZE_QVGA;   /* 320x240 */

/* The camera SCCB lines are the system I2C lines on this board. When that is
 * the case the esp32-camera driver must reuse the already-installed port
 * instead of installing its own, or the second i2c_driver_install() fails. */
static bool sccb_shares_system_bus(void)
{
    return BSP_CAM_PIN_SIOD == BSP_I2C_PIN_SDA && BSP_CAM_PIN_SIOC == BSP_I2C_PIN_SCL;
}

esp_err_t bsp_camera_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");

    /* Release PWDN if it is routed through the expander. */
    bsp_io_expander_set(CONFIG_BSP_IO_EXP_BIT_CAM_PWDN, false);

    const bool shared = sccb_shares_system_bus();

    camera_config_t cfg = {
        .pin_pwdn     = BSP_CAM_PIN_PWDN,
        .pin_reset    = BSP_CAM_PIN_RESET,
        .pin_xclk     = BSP_CAM_PIN_XCLK,
        .pin_sccb_sda = shared ? -1 : BSP_CAM_PIN_SIOD,
        .pin_sccb_scl = shared ? -1 : BSP_CAM_PIN_SIOC,
        .sccb_i2c_port = BSP_I2C_PORT,

        .pin_d0 = BSP_CAM_PIN_D0,
        .pin_d1 = BSP_CAM_PIN_D1,
        .pin_d2 = BSP_CAM_PIN_D2,
        .pin_d3 = BSP_CAM_PIN_D3,
        .pin_d4 = BSP_CAM_PIN_D4,
        .pin_d5 = BSP_CAM_PIN_D5,
        .pin_d6 = BSP_CAM_PIN_D6,
        .pin_d7 = BSP_CAM_PIN_D7,

        .pin_vsync = BSP_CAM_PIN_VSYNC,
        .pin_href  = BSP_CAM_PIN_HREF,
        .pin_pclk  = BSP_CAM_PIN_PCLK,

        .xclk_freq_hz = BSP_CAM_XCLK_FREQ_HZ,
        .ledc_timer   = LEDC_TIMER_1,      /* LEDC_TIMER_0 drives the backlight */
        .ledc_channel = LEDC_CHANNEL_1,

        /* JPEG for every consumer: the live view decodes it, the recorder
         * stores it verbatim, the detector decodes it 1:8. */
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = s_framesize,
        .jpeg_quality = 12,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        /* The OV5640 comes up upside down on this module. */
        s->set_vflip(s, 1);
        s->set_hmirror(s, 0);
        s->set_brightness(s, 0);
        s->set_saturation(s, 0);
        ESP_LOGI(TAG, "sensor PID 0x%04X", s->id.PID);
    }

    s_ready = true;
    ESP_LOGI(TAG, "camera ready (JPEG, framesize %d, SCCB %s)",
             (int)s_framesize, shared ? "shared with system I2C" : "dedicated");
    return ESP_OK;
}

esp_err_t bsp_camera_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }
    esp_err_t err = esp_camera_deinit();
    s_ready = false;
    bsp_io_expander_set(CONFIG_BSP_IO_EXP_BIT_CAM_PWDN, true);
    return err;
}

bool bsp_camera_is_ready(void)
{
    return s_ready;
}

sensor_t *bsp_camera_sensor(void)
{
    return s_ready ? esp_camera_sensor_get() : NULL;
}

esp_err_t bsp_camera_set_framesize(framesize_t size)
{
    sensor_t *s = bsp_camera_sensor();
    ESP_RETURN_ON_FALSE(s, ESP_ERR_INVALID_STATE, TAG, "camera not started");

    if (s->set_framesize(s, size) != 0) {
        return ESP_FAIL;
    }
    s_framesize = size;
    return ESP_OK;
}

framesize_t bsp_camera_get_framesize(void)
{
    return s_framesize;
}

esp_err_t bsp_camera_set_quality(int quality)
{
    sensor_t *s = bsp_camera_sensor();
    ESP_RETURN_ON_FALSE(s, ESP_ERR_INVALID_STATE, TAG, "camera not started");

    if (quality < 10) quality = 10;
    if (quality > 63) quality = 63;
    return s->set_quality(s, quality) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_camera_set_flip(bool hmirror, bool vflip)
{
    sensor_t *s = bsp_camera_sensor();
    ESP_RETURN_ON_FALSE(s, ESP_ERR_INVALID_STATE, TAG, "camera not started");

    s->set_hmirror(s, hmirror ? 1 : 0);
    s->set_vflip(s, vflip ? 1 : 0);
    return ESP_OK;
}
