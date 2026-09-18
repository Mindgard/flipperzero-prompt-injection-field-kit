#pragma once

#include "../pifk_app.h"

/* Payload egress over the GPIO header's I2C bus.
 *
 * The UART channel reaches targets with a serial console. This one
 * reaches the ones without: an EEPROM holding configuration text, a
 * display controller's frame buffer, a sensor's register space. Those
 * turn up on embedded hardware that exposes no console at all, and an
 * LLM-assisted pipeline that later reads device configuration or
 * summarises a display's contents becomes the consumer.
 *
 * Wiring, with pin numbers resolved at runtime rather than hardcoded:
 *
 *   Flipper SCL (PC0)  ->  target SCL
 *   Flipper SDA (PC1)  ->  target SDA
 *   Flipper GND        ->  target GND     (required)
 *
 * Pull-ups are the target board's job. The Flipper's I2C pins float on
 * release and the HAL does not enable internal pull-ups, so a bus with
 * no pull-up resistors reads as every address being absent — which looks
 * exactly like nothing being connected. Most real boards have them.
 *
 * 3.3V. A 5V bus needs a level shifter.
 *
 * ── Master only ──────────────────────────────────────────────── *
 *
 * The exported HAL is master-side: furi_hal_i2c_tx() addresses a slave
 * and there is no slave-mode API. So the kit writes a payload *into* a
 * target that behaves as a slave; it cannot impersonate a sensor that a
 * target polls. That is a real limit on the channel and worth knowing
 * before wiring: if the target is the bus master, this channel has
 * nothing to say to it.
 */

/* Address range scanned. 0x00-0x07 and 0x78-0x7F are reserved by the
 * I2C specification (general call, 10-bit addressing, and so on), so
 * probing them is both pointless and capable of confusing a device. */
#define PIFK_I2C_ADDR_MIN 0x08
#define PIFK_I2C_ADDR_MAX 0x77

/* Responding addresses retained by a scan. A bus with more devices than
 * this is unusual; the count is reported so a full bus is visible rather
 * than silently clipped. */
#define PIFK_I2C_MAX_FOUND 16

/* Per-transfer timeout. Short: a device that does not ACK its address
 * fails immediately, and the scan multiplies this by 112 addresses. */
#define PIFK_I2C_TIMEOUT_MS 5

/* Bytes per write transaction. A payload longer than this is split
 * across several transfers, each re-addressing the slave, because an
 * arbitrary device's receive buffer is unknown and a 512-byte single
 * transaction would overrun most of them. */
#define PIFK_I2C_CHUNK 16

typedef struct {
    uint8_t addr[PIFK_I2C_MAX_FOUND];
    uint8_t count; /* entries in addr[] */
    uint8_t total; /* responding addresses seen, may exceed count */
} PifkI2cScan;

/* Probe every address in the scan range.
 *
 * Returns true if at least one device responded. The scan is the useful
 * half of this channel in the field: writing to a guessed address is how
 * you brick a device you did not mean to touch, and the payload write
 * refuses an address that did not respond unless forced. */
bool pifk_i2c_scan(PifkApp* app, PifkI2cScan* out);

/* Write `payload` to the configured I2C address, in
 * PIFK_I2C_CHUNK-sized transactions.
 *
 * Checks app->executing between chunks, so Stop interrupts a transfer
 * part-way through as it does on the paced UART path.
 *
 * Returns false if the payload is empty, the address did not ACK, or a
 * transfer failed; see pifk_i2c_last_error(). A partial write is
 * reported as a failure, but the bytes already sent have of course
 * arrived — an I2C write cannot be rolled back. */
bool pifk_i2c_execute(PifkApp* app, const PifkPayload* payload);

/* Bytes delivered by the last pifk_i2c_execute() call, including a
 * partial write that then failed. */
size_t pifk_i2c_bytes_written(void);

/* Human-readable reason the last call failed, or NULL. Points at static
 * storage; valid until the next I2C call. */
const char* pifk_i2c_last_error(void);

/* Physical header pin numbers for SCL and SDA, per this firmware. Either
 * pointer may be NULL. Values are -1 if the signal is not on the
 * external connector. */
void pifk_i2c_pins(int32_t* scl_pin, int32_t* sda_pin);
