#pragma once

#include "../pifk_app.h"

/* Send payload text as a byte stream on the GPIO header's UART.
 *
 * Unlike the other channels, this one does not speak a target's
 * protocol — it emits bytes and leaves the receiving end to whatever
 * the user has wired up. That makes it the way into targets the kit
 * cannot anticipate: a serial console on an industrial HMI, a kiosk
 * debug header, a robot's UART, a custom pipeline feeding an LLM.
 *
 * The kit deliberately knows nothing about what is downstream.
 *
 * Wiring: connect the Flipper's UART TX to the target's RX and share a
 * ground. GND is not optional — a floating ground produces garbage that
 * looks exactly like a software bug. Pin numbers are resolved at
 * runtime via furi_hal_resources_get_ext_pin_number() rather than
 * hardcoded, so the UI always shows what this firmware actually means.
 *
 * The Flipper's GPIO is 3.3V. Feeding 5V logic in can damage the MCU;
 * a 5V target needs a level shifter. */

typedef enum {
    PifkGpioLineEndingNone = 0,
    PifkGpioLineEndingLf,
    PifkGpioLineEndingCrLf,
    PifkGpioLineEndingCount,
} PifkGpioLineEnding;

/* Upper bound on the paced inter-byte delay, so a persisted or remote
 * value cannot stall the app for an unreasonable time. */
#define PIFK_GPIO_MAX_BYTE_DELAY_MS 100

/* Send `payload` over the configured UART.
 *
 * With app->gpio_byte_delay_ms == 0 the whole buffer is handed to the
 * driver at once. Non-zero paces the send one byte at a time, checking
 * app->executing between bytes — so unlike BadUSB, Stop can interrupt a
 * transfer part-way through. Pacing also helps receivers with small
 * buffers, since the exported API has no RTS/CTS flow control.
 *
 * Returns false if the payload is empty or the UART could not be
 * acquired (the CLI and logging own the USART by default; see
 * pifk_gpio_last_error). */
bool pifk_execute_gpio(PifkApp* app, const PifkPayload* payload);
/* Human-readable reason the last call failed, or NULL if it succeeded.
 * Points at static storage; valid until the next GPIO call. */
const char* pifk_gpio_last_error(void);

/* Loopback self-test.
 *
 * Wire the UART TX pin to the RX pin and this sends a known string,
 * reads it back and compares. It is the only channel in the kit that
 * can prove its transport works before the user's hardware is involved,
 * which turns "it didn't work" into a two-way diagnosis.
 *
 * On return, *sent and *received hold the byte counts. Returns true
 * only if every byte came back identical. */
bool pifk_gpio_loopback_test(PifkApp* app, size_t* sent, size_t* received);

/* Physical header pin numbers for the configured UART, per this
 * firmware. Either pointer may be NULL. Values are -1 if the signal is
 * not on the external connector. */
void pifk_gpio_pins(const PifkApp* app, int32_t* tx_pin, int32_t* rx_pin);

/* ── Response capture ────────────────────────────────────────── *
 *
 * Every other channel in this kit is write-only: it reports that a
 * payload was delivered, never whether it had any effect. If the target
 * has a serial console, its reply to the payload is the actual evidence
 * — a leaked system prompt read back off the wire is worth more than a
 * delivery confirmation.
 *
 * Capture is opt-in per send. RX must be wired for it to see anything;
 * with only TX connected the result is simply an empty response, which
 * is reported as such rather than as a failure. */

/* Bytes retained from the target's reply. Sized to hold a system-prompt
 * disclosure without growing the app struct unreasonably. */
#define PIFK_GPIO_RX_CAPACITY 512

/* Send `payload`, then listen for a reply for `listen_ms`.
 *
 * The captured bytes are available via pifk_gpio_response() once
 * this returns. Non-printable bytes are kept as-is in the buffer;
 * rendering is the caller's problem.
 *
 * Returns the send result. A successful send with no reply still
 * returns true — "the target said nothing" is a finding, not an error. */
bool pifk_execute_gpio_capture(PifkApp* app, const PifkPayload* payload, uint32_t listen_ms);

/* The most recent captured response. Returns a NUL-terminated buffer and
 * writes the byte count to *len if non-NULL. Valid until the next GPIO
 * call. Never NULL; empty string when nothing was captured. */
const char* pifk_gpio_response(size_t* len);

/* True if the last capture filled the buffer, meaning the reply was
 * truncated and the target may have said more. */
bool pifk_gpio_response_truncated(void);
