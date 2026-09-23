/* MFRC522 contactless reader, 13.56 MHz.
 *
 * The reader shares the LCD's SPI bus and adds only a chip select, which is
 * the whole reason it was chosen: this board has two free GPIOs and the door
 * strike wants one of them.
 *
 * Scope: this reads the UID during anticollision and stops there. It does not
 * authenticate a MIFARE sector, so the credential it produces is the card's
 * serial number and nothing more. A UID is public, unauthenticated and
 * trivially copied by any cloner -- treat it as an identifier, not a secret.
 *
 * Doing better means a card that can prove it holds a key: MIFARE Classic
 * sector authentication (weak, but not free to clone) or DESFire with AES.
 * Both need a crypto exchange after SELECT, which is where a future
 * bsp_rfid_authenticate() would slot in -- the UID path above stays as it is.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest UID the standard allows: 4, 7 or 10 bytes (cascade levels 1..3). */
#define BSP_RFID_UID_MAX    10

typedef struct {
    uint8_t bytes[BSP_RFID_UID_MAX];
    uint8_t len;            /* 4, 7 or 10 */
    uint8_t sak;            /* select acknowledge, identifies the card family */
    uint16_t atqa;          /* answer to request */
} bsp_rfid_uid_t;

/**
 * Adds the reader to the LCD's SPI bus and resets it.
 *
 * bsp_display_init() must have run first -- it is what calls
 * spi_bus_initialize(), and without a bus there is nothing to attach to.
 * Returns ESP_ERR_NOT_SUPPORTED when the reader is disabled in Kconfig.
 */
esp_err_t bsp_rfid_init(void);

/** True once a chip answered with a plausible version at init. */
bool bsp_rfid_present(void);

/** The VersionReg value read at init: 0x91/0x92 for a genuine MFRC522. */
uint8_t bsp_rfid_chip_version(void);

/**
 * Looks for a card in the field and reads its UID.
 *
 * Returns ESP_OK when one answered, ESP_ERR_NOT_FOUND when the field is
 * empty (the normal case, and not worth logging), or another error when the
 * exchange started but did not complete.
 */
esp_err_t bsp_rfid_poll(bsp_rfid_uid_t *out);

/** Puts the selected card back to sleep so the next poll re-detects it. */
void bsp_rfid_halt(void);

/** Turns the 13.56 MHz carrier on or off. Off saves ~100 mW. */
esp_err_t bsp_rfid_antenna(bool on);

/**
 * Formats a UID as uppercase hex, e.g. "04A2B31C". `out` needs
 * BSP_RFID_UID_MAX*2+1 bytes.
 */
void bsp_rfid_uid_str(const bsp_rfid_uid_t *uid, char *out, size_t out_len);

/** True when two UIDs are the same length and the same bytes. */
bool bsp_rfid_uid_equal(const bsp_rfid_uid_t *a, const bsp_rfid_uid_t *b);

#ifdef __cplusplus
}
#endif
