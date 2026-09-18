#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Minimal QR Code encoder for Flipper Zero.
 *
 * Supports QR Code Model 2, byte mode, ECC level L,
 * versions 1–6 (21–41 modules).
 *
 * Max data capacity (byte mode, ECC-L):
 *   V1=17  V2=32  V3=53  V4=78  V5=106  V6=134
 */

#define QR_MAX_VERSION 6
#define QR_MAX_MODULES (17 + QR_MAX_VERSION * 4) /* 41 for V6 */
#define QR_MAX_BYTES   ((QR_MAX_MODULES * QR_MAX_MODULES + 7) / 8)

typedef struct {
    uint8_t version; /* 1–6 */
    uint8_t size; /* modules per side (21–33) */
    uint8_t modules[QR_MAX_MODULES][QR_MAX_MODULES]; /* 0=white, 1=black */
    bool ok;
} QrCode;

/* Encode `data` (length `len`) into a QR code.
 * Returns a heap-allocated QrCode (caller must free).
 * Returns NULL on allocation failure; ok=false if data too long. */
QrCode* qrcode_encode(const uint8_t* data, size_t len);
