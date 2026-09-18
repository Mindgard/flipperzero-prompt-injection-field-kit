#pragma once

/*
 * Channel capacity limits, free of any Flipper HAL dependency.
 *
 * The executors' own headers each include pifk_app.h, and
 * nfc_listener_exec.h pulls in the NFC protocol stack, so a host-side
 * test cannot include them without stubbing the whole HAL.  These are
 * the numbers the eligibility filter decides on, so they live here
 * where tests/test_channel.c can include them directly and verify the
 * real values rather than a hand-copied set that can drift.
 *
 * Each limit is still derived from, and asserted against, the definition
 * next to the code enforcing it — see the _Static_asserts in channel.c.
 * This header is a HAL-free mirror, not a second source of truth.
 */

/* BadUSB, GPIO/UART, I2C and the BLE beacon are all bounded by the
 * payload text buffer rather than by the channel. */
#define PIFK_LIMIT_TEXT_BUF 512

/* QR v6, ECC-L, byte mode. */
#define PIFK_LIMIT_QR 134

/* Manufacturer + product + serial, 126 characters each. */
#define PIFK_LIMIT_USBDESC 378

/* NTAG215 declares a 496-byte NDEF area; a text record costs 15. */
#define PIFK_LIMIT_NFC 481

/* Three characteristics of 244 bytes. */
#define PIFK_LIMIT_GATT 732
