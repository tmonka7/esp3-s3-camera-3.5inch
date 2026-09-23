#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_board.h"
#include "bsp_rfid.h"

static const char *TAG = "rc522";

/* ---- registers (datasheet 9.2, addresses are already byte offsets) ------ */
#define REG_COMMAND         0x01
#define REG_COM_IRQ         0x04
#define REG_DIV_IRQ         0x05
#define REG_ERROR           0x06
#define REG_FIFO_DATA       0x09
#define REG_FIFO_LEVEL      0x0A
#define REG_CONTROL         0x0C
#define REG_BIT_FRAMING     0x0D
#define REG_COLL            0x0E
#define REG_MODE            0x11
#define REG_TX_MODE         0x12
#define REG_RX_MODE         0x13
#define REG_TX_CONTROL      0x14
#define REG_TX_ASK          0x15
#define REG_CRC_RESULT_H    0x21
#define REG_CRC_RESULT_L    0x22
#define REG_MOD_WIDTH       0x24
#define REG_T_MODE          0x2A
#define REG_T_PRESCALER     0x2B
#define REG_T_RELOAD_H      0x2C
#define REG_T_RELOAD_L      0x2D
#define REG_VERSION         0x37

/* ---- PCD (reader) commands --------------------------------------------- */
#define CMD_IDLE            0x00
#define CMD_CALC_CRC        0x03
#define CMD_TRANSCEIVE      0x0C
#define CMD_SOFT_RESET      0x0F

/* ---- PICC (card) commands ---------------------------------------------- */
#define PICC_REQA           0x26
#define PICC_HLTA           0x50
#define PICC_CASCADE_TAG    0x88
#define PICC_SEL_CL1        0x93
#define PICC_SEL_CL2        0x95
#define PICC_SEL_CL3        0x97

/* ErrorReg bits worth failing on: buffer overflow, parity, protocol. */
#define ERR_MASK            0x13

static spi_device_handle_t s_spi;
static bool                s_present;
static uint8_t             s_version;

/* --------------------------------------------------------------------------
 * Register access
 *
 * The MFRC522 SPI address byte is (addr << 1) with bit 7 set for a read and
 * bit 0 always zero -- not the raw register number, which is the single most
 * common way to get this chip wrong.
 * ------------------------------------------------------------------------ */
static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t             tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t   t     = {
        .length    = 16,
        .tx_buffer = tx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    uint8_t           tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    uint8_t           rx[2] = { 0 };
    spi_transaction_t t     = {
        .length    = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    const esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err == ESP_OK) {
        *val = rx[1];
    }
    return err;
}

static esp_err_t reg_write_buf(uint8_t reg, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const esp_err_t err = reg_write(reg, data[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void reg_set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v = 0;
    if (reg_read(reg, &v) == ESP_OK) {
        reg_write(reg, (uint8_t)(v | mask));
    }
}

static void reg_clear_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v = 0;
    if (reg_read(reg, &v) == ESP_OK) {
        reg_write(reg, (uint8_t)(v & ~mask));
    }
}

/* --------------------------------------------------------------------------
 * CRC_A, computed by the chip rather than in software
 * ------------------------------------------------------------------------ */
static esp_err_t calc_crc(const uint8_t *data, size_t len, uint8_t *out)
{
    reg_write(REG_COMMAND, CMD_IDLE);
    reg_write(REG_DIV_IRQ, 0x04);          /* clear CRCIRq            */
    reg_write(REG_FIFO_LEVEL, 0x80);       /* flush FIFO              */
    ESP_RETURN_ON_ERROR(reg_write_buf(REG_FIFO_DATA, data, len), TAG, "crc fifo");
    reg_write(REG_COMMAND, CMD_CALC_CRC);

    /* The calculation is a few dozen microseconds; 90 ms is a generous bound
     * that only matters if the chip has stopped answering. */
    for (int i = 0; i < 90; i++) {
        uint8_t irq = 0;
        if (reg_read(REG_DIV_IRQ, &irq) == ESP_OK && (irq & 0x04)) {
            reg_write(REG_COMMAND, CMD_IDLE);
            ESP_RETURN_ON_ERROR(reg_read(REG_CRC_RESULT_L, &out[0]), TAG, "crc lo");
            ESP_RETURN_ON_ERROR(reg_read(REG_CRC_RESULT_H, &out[1]), TAG, "crc hi");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_ERR_TIMEOUT;
}

/* --------------------------------------------------------------------------
 * One transceive exchange with a card
 *
 * tx_last_bits is how many bits of the final byte to send: 0 means all eight,
 * 7 is the short-frame format REQA needs. rx_align matters only during
 * anticollision, where a partial byte has to be merged with what we already
 * know.
 * ------------------------------------------------------------------------ */
static esp_err_t transceive(const uint8_t *tx, size_t tx_len,
                            uint8_t *rx, size_t *rx_len,
                            uint8_t *rx_last_bits,
                            uint8_t tx_last_bits, uint8_t rx_align)
{
    const uint8_t bit_framing = (uint8_t)((rx_align << 4) | tx_last_bits);

    reg_write(REG_COMMAND, CMD_IDLE);
    reg_write(REG_COM_IRQ, 0x7F);          /* clear every interrupt bit */
    reg_write(REG_FIFO_LEVEL, 0x80);       /* flush FIFO                */
    ESP_RETURN_ON_ERROR(reg_write_buf(REG_FIFO_DATA, tx, tx_len), TAG, "tx fifo");
    reg_write(REG_BIT_FRAMING, bit_framing);
    reg_write(REG_COMMAND, CMD_TRANSCEIVE);
    reg_set_bits(REG_BIT_FRAMING, 0x80);   /* StartSend                 */

    /* The chip's own timer is armed for ~25 ms (see the reload values in
     * bsp_rfid_init), so TimerIRq is the authority on "no card answered".
     * The loop bound below only catches a chip that has stopped responding
     * altogether, which is why it is comfortably longer. */
    uint8_t irq = 0;
    bool    done = false;
    for (int i = 0; i < 60; i++) {
        if (reg_read(REG_COM_IRQ, &irq) != ESP_OK) {
            return ESP_FAIL;
        }
        if (irq & 0x30) {                  /* RxIRq | IdleIRq           */
            done = true;
            break;
        }
        if (irq & 0x01) {                  /* TimerIRq: nothing there   */
            return ESP_ERR_NOT_FOUND;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!done) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t err_reg = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_ERROR, &err_reg), TAG, "error reg");
    if (err_reg & ERR_MASK) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!rx || !rx_len) {
        return ESP_OK;
    }

    uint8_t level = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_FIFO_LEVEL, &level), TAG, "fifo level");
    if (level > *rx_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint8_t i = 0; i < level; i++) {
        ESP_RETURN_ON_ERROR(reg_read(REG_FIFO_DATA, &rx[i]), TAG, "rx fifo");
    }
    *rx_len = level;

    if (rx_last_bits) {
        uint8_t ctrl = 0;
        ESP_RETURN_ON_ERROR(reg_read(REG_CONTROL, &ctrl), TAG, "control");
        *rx_last_bits = (uint8_t)(ctrl & 0x07);
    }
    return ESP_OK;
}

/** REQA. A card in the field replies with a 2-byte ATQA. */
static esp_err_t request_a(uint16_t *atqa)
{
    /* Anticollision must be allowed to run; a stale CollReg ValuesAfterColl
     * bit makes the first select after a collision fail. */
    reg_clear_bits(REG_COLL, 0x80);

    uint8_t   cmd    = PICC_REQA;
    uint8_t   buf[2] = { 0 };
    size_t    len    = sizeof(buf);
    uint8_t   valid  = 0;

    const esp_err_t err = transceive(&cmd, 1, buf, &len, &valid, 7, 0);
    if (err != ESP_OK) {
        return err;
    }
    /* ATQA is exactly 16 bits; anything else is a collision or a misread. */
    if (len != 2 || valid != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *atqa = (uint16_t)((buf[1] << 8) | buf[0]);
    return ESP_OK;
}

/**
 * Runs one cascade level: full anticollision then SELECT.
 *
 * Returns the 4 UID bytes of this level plus the SAK. This is the simplified
 * form that assumes a single card in the field -- it sends the complete
 * 32-bit anticollision request rather than walking the collision tree bit by
 * bit. Two cards presented at once will collide and simply not read, which
 * for a door is the right failure: nothing opens.
 */
static esp_err_t cascade_level(uint8_t sel_cmd, uint8_t *uid4, uint8_t *sak)
{
    /* NVB 0x20: "I am sending 2 bytes", i.e. ask for the whole UID. */
    uint8_t anticoll[2] = { sel_cmd, 0x20 };
    uint8_t resp[5]     = { 0 };
    size_t  resp_len    = sizeof(resp);

    ESP_RETURN_ON_ERROR(transceive(anticoll, sizeof(anticoll), resp, &resp_len,
                                   NULL, 0, 0),
                        TAG, "anticollision");
    if (resp_len != 5) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* BCC is the XOR of the four UID bytes; a mismatch means a collision or
     * a corrupted read, never a valid card. */
    const uint8_t bcc = (uint8_t)(resp[0] ^ resp[1] ^ resp[2] ^ resp[3]);
    if (bcc != resp[4]) {
        return ESP_ERR_INVALID_CRC;
    }
    memcpy(uid4, resp, 4);

    /* SELECT: NVB 0x70 (7 bytes), the UID, the BCC and a CRC_A. */
    uint8_t sel[9] = { sel_cmd, 0x70, resp[0], resp[1], resp[2], resp[3], resp[4] };
    ESP_RETURN_ON_ERROR(calc_crc(sel, 7, &sel[7]), TAG, "select crc");

    uint8_t sak_resp[3] = { 0 };
    size_t  sak_len     = sizeof(sak_resp);
    ESP_RETURN_ON_ERROR(transceive(sel, sizeof(sel), sak_resp, &sak_len, NULL, 0, 0),
                        TAG, "select");
    if (sak_len != 3) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t crc[2] = { 0 };
    ESP_RETURN_ON_ERROR(calc_crc(sak_resp, 1, crc), TAG, "sak crc");
    if (crc[0] != sak_resp[1] || crc[1] != sak_resp[2]) {
        return ESP_ERR_INVALID_CRC;
    }

    *sak = sak_resp[0];
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */
esp_err_t bsp_rfid_init(void)
{
#if !BSP_RC522_ENABLED
    ESP_LOGI(TAG, "reader disabled in Kconfig");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_spi) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(BSP_RC522_PIN_CS >= 0, ESP_ERR_INVALID_STATE, TAG,
                        "no chip select configured");

    if (BSP_RC522_PIN_RST >= 0) {
        const gpio_config_t rst = {
            .mode         = GPIO_MODE_OUTPUT,
            /* Masked for the same reason as the strike and buzzer pins: this
             * is a compile-time constant that is -1 by default, and the
             * compiler folds the shift even on the branch the guard excludes. */
            .pin_bit_mask = 1ULL << (BSP_RC522_PIN_RST & 0x3F),
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "reset gpio");
        gpio_set_level((gpio_num_t)BSP_RC522_PIN_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        gpio_set_level((gpio_num_t)BSP_RC522_PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    const spi_device_interface_config_t dev = {
        .clock_speed_hz = BSP_RC522_SPI_CLOCK_HZ,
        .mode           = 0,
        .spics_io_num   = BSP_RC522_PIN_CS,
        .queue_size     = 1,
    };
    /* The display owns the bus and has already initialised it; this just
     * attaches a second, much slower device. The SPI driver reprograms the
     * clock per transaction, so the LCD keeps its own speed. */
    ESP_RETURN_ON_ERROR(spi_bus_add_device(BSP_LCD_SPI_HOST, &dev, &s_spi), TAG,
                        "add device (is the display up?)");

    reg_write(REG_COMMAND, CMD_SOFT_RESET);
    /* The reset bit clears itself; the datasheet allows 37.74 us but a real
     * board with a slow crystal start-up wants more slack than that. */
    vTaskDelay(pdMS_TO_TICKS(50));

    reg_write(REG_TX_MODE, 0x00);
    reg_write(REG_RX_MODE, 0x00);
    reg_write(REG_MOD_WIDTH, 0x26);

    /* Timer: prescaler 0xA9 with a 0x03E8 reload gives ~25 ms, the usual
     * value, which is what makes TimerIRq a reliable "no card" answer. */
    reg_write(REG_T_MODE, 0x80);
    reg_write(REG_T_PRESCALER, 0xA9);
    reg_write(REG_T_RELOAD_H, 0x03);
    reg_write(REG_T_RELOAD_L, 0xE8);

    reg_write(REG_TX_ASK, 0x40);           /* Force100ASK               */
    reg_write(REG_MODE, 0x3D);             /* CRC preset 0x6363         */

    if (reg_read(REG_VERSION, &s_version) != ESP_OK) {
        ESP_LOGE(TAG, "no answer on SPI");
        return ESP_ERR_NOT_FOUND;
    }

    /* 0x91/0x92 are the genuine NXP silicon revisions. Clones report all
     * sorts of things and mostly work, so this warns rather than refuses --
     * but 0x00 and 0xFF mean the wiring is wrong, not that the chip is odd. */
    if (s_version == 0x00 || s_version == 0xFF) {
        ESP_LOGE(TAG, "VersionReg reads 0x%02X -- check CS, MISO and 3V3",
                 s_version);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_version != 0x91 && s_version != 0x92) {
        ESP_LOGW(TAG, "unexpected VersionReg 0x%02X (clone?), continuing",
                 s_version);
    }

    ESP_RETURN_ON_ERROR(bsp_rfid_antenna(true), TAG, "antenna");

    s_present = true;
    ESP_LOGI(TAG, "MFRC522 ready on CS %d @ %d Hz (version 0x%02X)",
             BSP_RC522_PIN_CS, BSP_RC522_SPI_CLOCK_HZ, s_version);
    return ESP_OK;
#endif
}

bool bsp_rfid_present(void)
{
    return s_present;
}

uint8_t bsp_rfid_chip_version(void)
{
    return s_version;
}

esp_err_t bsp_rfid_antenna(bool on)
{
    if (!s_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on) {
        reg_set_bits(REG_TX_CONTROL, 0x03);
    } else {
        reg_clear_bits(REG_TX_CONTROL, 0x03);
    }
    return ESP_OK;
}

esp_err_t bsp_rfid_poll(bsp_rfid_uid_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out");
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));

    uint16_t        atqa = 0;
    const esp_err_t err  = request_a(&atqa);
    if (err != ESP_OK) {
        /* An empty field is the overwhelmingly common case and reports as
         * NOT_FOUND so the caller can ignore it without filtering. */
        return err;
    }
    out->atqa = atqa;

    /* Up to three cascade levels. Level 1 returns four bytes; if the first is
     * the cascade tag then those are only three real UID bytes plus a marker
     * and the next level holds the rest. */
    static const uint8_t sel_cmds[3] = { PICC_SEL_CL1, PICC_SEL_CL2, PICC_SEL_CL3 };

    uint8_t len = 0;
    for (int level = 0; level < 3; level++) {
        uint8_t four[4] = { 0 };
        uint8_t sak     = 0;

        ESP_RETURN_ON_ERROR(cascade_level(sel_cmds[level], four, &sak), TAG,
                            "cascade %d", level + 1);

        if (four[0] == PICC_CASCADE_TAG) {
            /* Three real bytes this level, more to come. */
            if (len + 3 > BSP_RFID_UID_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(&out->bytes[len], &four[1], 3);
            len = (uint8_t)(len + 3);
        } else {
            if (len + 4 > BSP_RFID_UID_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(&out->bytes[len], four, 4);
            len = (uint8_t)(len + 4);
        }

        /* SAK bit 2 set means the UID is still incomplete. */
        if (!(sak & 0x04)) {
            out->sak = sak;
            out->len = len;
            return ESP_OK;
        }
    }

    /* Three levels without a complete UID is not a card the standard allows. */
    return ESP_ERR_INVALID_RESPONSE;
}

void bsp_rfid_halt(void)
{
    if (!s_present) {
        return;
    }
    uint8_t hlta[4] = { PICC_HLTA, 0x00 };
    if (calc_crc(hlta, 2, &hlta[2]) != ESP_OK) {
        return;
    }
    /* A card that accepts HLTA answers with nothing at all, so the timeout
     * here is the success path and there is no status worth checking. */
    (void)transceive(hlta, sizeof(hlta), NULL, NULL, NULL, 0, 0);
}

void bsp_rfid_uid_str(const bsp_rfid_uid_t *uid, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!uid) {
        return;
    }

    size_t pos = 0;
    for (uint8_t i = 0; i < uid->len && pos + 3 <= out_len; i++) {
        static const char hex[] = "0123456789ABCDEF";
        out[pos++] = hex[(uid->bytes[i] >> 4) & 0x0F];
        out[pos++] = hex[uid->bytes[i] & 0x0F];
    }
    out[pos] = '\0';
}

bool bsp_rfid_uid_equal(const bsp_rfid_uid_t *a, const bsp_rfid_uid_t *b)
{
    if (!a || !b || a->len != b->len || a->len == 0) {
        return false;
    }
    return memcmp(a->bytes, b->bytes, a->len) == 0;
}
