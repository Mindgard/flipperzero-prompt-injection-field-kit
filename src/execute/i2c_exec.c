/*
 * Payload egress over the GPIO header's I2C bus.
 *
 * See i2c_exec.h for what this channel reaches and why it is master-only.
 *
 * Three things here need care rather than cleverness:
 *
 *   1. The bus handle must be released on every exit path.
 *      furi_hal_i2c_acquire() takes a mutex and blocks other users —
 *      including the firmware's own power management, which shares the
 *      I2C peripheral driver — so leaking it is worse than leaking the
 *      UART handle. Every return below goes through i2c_release().
 *
 *   2. Writing to a guessed address is destructive in a way the UART
 *      channel is not. An I2C slave at an address you did not expect
 *      might be a PMIC, and payload text written into its control
 *      registers is a bricked board. The write path therefore probes the
 *      address first and refuses if nothing ACKs.
 *
 *   3. Transfers are chunked. I2C is transactional and an unknown
 *      device's receive buffer is, by definition, unknown; a 512-byte
 *      single write would overrun most real parts. See
 *      PIFK_I2C_CHUNK.
 */

#include "i2c_exec.h"
#include <furi_hal_i2c.h>
#include <furi_hal_resources.h>
#include <string.h>

static const char* i2c_error = NULL;
static size_t i2c_written = 0;

const char* pifk_i2c_last_error(void) {
    return i2c_error;
}

size_t pifk_i2c_bytes_written(void) {
    return i2c_written;
}

void pifk_i2c_pins(int32_t* scl_pin, int32_t* sda_pin) {
    /* Ask the firmware for the header numbering rather than hardcoding
     * it, as the UART channel does: this cannot drift, and it stays
     * correct if the numbering differs across hardware revisions.
     * External I2C is I2C3 on PC0 (SCL) / PC1 (SDA). */
    if(scl_pin) *scl_pin = furi_hal_resources_get_ext_pin_number(&gpio_ext_pc0);
    if(sda_pin) *sda_pin = furi_hal_resources_get_ext_pin_number(&gpio_ext_pc1);
}

/* ── Acquire / release ───────────────────────────────────────── */

static void i2c_acquire(void) {
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
}

static void i2c_release(void) {
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
}

/* The HAL takes a 7-bit address pre-shifted into bits 7:1, which is the
 * form that goes on the wire. Callers and the UI work in the
 * conventional unshifted 7-bit form, so convert in exactly one place. */
static uint8_t i2c_wire_addr(uint8_t addr7) {
    return (uint8_t)(addr7 << 1);
}

/* ── Bus scan ────────────────────────────────────────────────── */

bool pifk_i2c_scan(PifkApp* app, PifkI2cScan* out) {
    i2c_error = NULL;
    if(!out) return false;
    memset(out, 0, sizeof(*out));

    i2c_acquire();

    app->executing = true;
    for(uint8_t a = PIFK_I2C_ADDR_MIN; a <= PIFK_I2C_ADDR_MAX; a++) {
        if(!app->executing) break; /* Stop pressed */

        if(furi_hal_i2c_is_device_ready(
               &furi_hal_i2c_handle_external, i2c_wire_addr(a), PIFK_I2C_TIMEOUT_MS)) {
            /* total counts everything seen; addr[] holds what fits, so a
             * crowded bus is reported rather than silently clipped. */
            if(out->count < PIFK_I2C_MAX_FOUND) {
                out->addr[out->count++] = a;
            }
            out->total++;
        }
    }
    app->executing = false;

    i2c_release();

    if(out->total == 0) {
        /* Naming the pull-up case first: it is the most common cause and
         * the one that looks identical to an empty bus. */
        i2c_error = "No devices. Check pull-ups, wiring and GND.";
        return false;
    }
    return true;
}

/* ── Payload write ───────────────────────────────────────────── */

bool pifk_i2c_execute(PifkApp* app, const PifkPayload* payload) {
    i2c_error = NULL;
    i2c_written = 0;

    if(!payload || !payload->text || !payload->text[0]) {
        i2c_error = "Payload is empty.";
        return false;
    }

    uint8_t addr7 = app->i2c_address;
    if(addr7 < PIFK_I2C_ADDR_MIN || addr7 > PIFK_I2C_ADDR_MAX) {
        i2c_error = "Address is outside the 0x08-0x77 range.";
        return false;
    }

    const uint8_t wire = i2c_wire_addr(addr7);
    size_t len = strlen(payload->text);

    i2c_acquire();

    /* Probe before writing. An address that does not ACK is either
     * absent or not listening, and writing anyway means guessing at what
     * hardware is on the other end. */
    if(!furi_hal_i2c_is_device_ready(&furi_hal_i2c_handle_external, wire, PIFK_I2C_TIMEOUT_MS)) {
        i2c_release();
        i2c_error = "Nothing ACKed that address. Scan the bus first.";
        return false;
    }

    app->executing = true;
    bool complete = true;

    for(size_t off = 0; off < len;) {
        if(!app->executing) {
            complete = false;
            break;
        }

        size_t take = len - off;
        if(take > PIFK_I2C_CHUNK) take = PIFK_I2C_CHUNK;

        if(!furi_hal_i2c_tx(
               &furi_hal_i2c_handle_external,
               wire,
               (const uint8_t*)payload->text + off,
               take,
               PIFK_I2C_TIMEOUT_MS)) {
            complete = false;
            /* Distinguish "refused from the start" from "stopped ACKing
             * part-way", because they mean different things: a wrong
             * address versus a full receive buffer. */
            i2c_error = (off == 0) ? "Device refused the first write." :
                                     "Device stopped ACKing part-way through.";
            break;
        }

        off += take;
        i2c_written = off;
    }

    app->executing = false;
    i2c_release();

    if(!complete && !i2c_error) {
        i2c_error = "Stopped before the payload finished.";
    }
    return complete;
}
