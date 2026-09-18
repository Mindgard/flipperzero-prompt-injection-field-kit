/*
 * Payload egress over the GPIO header's UART.
 *
 * The other channels each speak a specific protocol to a specific kind
 * of target. This one does not: it emits bytes on a pin and lets the
 * user's own hardware own the protocol. That inversion is what makes it
 * useful — it reaches targets the kit cannot anticipate — and also what
 * makes it cheap, since there is no protocol to get wrong on our side.
 *
 * Two things here need care rather than cleverness:
 *
 *   1. The serial handle must be released on every exit path. The CLI
 *      and logging own the USART by default, and leaking the handle
 *      leaves them dead until reboot. Every return below goes through
 *      gpio_release().
 *
 *   2. furi_hal_serial_control_acquire() returns NULL when the
 *      interface is busy rather than clobbering it. That is a
 *      *feature*: contention is detectable. The UI has to explain it,
 *      though, or "failed" is useless — hence pifk_gpio_last_error().
 */

#include "gpio_exec.h"
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <furi_hal_resources.h>
#include <string.h>

#define GPIO_LOOPBACK_PROBE      "PIFK-GPIO-LOOPBACK-0123456789"
#define GPIO_LOOPBACK_TIMEOUT_MS 500

static const char* gpio_error = NULL;

/* Loopback receive state. The RX callback runs in interrupt context, so
 * it does the minimum: pull the byte and stash it. */
typedef struct {
    uint8_t buf[64];
    volatile size_t len;
} GpioRxCapture;

static GpioRxCapture gpio_rx;

/* Response capture. Separate from the loopback buffer because the two
 * have different sizes and lifetimes, and sharing them would mean a
 * loopback test silently discarding a captured response.
 *
 * `len` is written only by the ISR and read only by the main thread
 * after RX has been stopped, so no lock is needed — but the ISR must
 * never block, hence a fixed buffer rather than an allocation. */
static struct {
    char buf[PIFK_GPIO_RX_CAPACITY + 1];
    volatile size_t len;
    volatile bool overflow;
} gpio_resp;

const char* pifk_gpio_last_error(void) {
    return gpio_error;
}

static FuriHalSerialId gpio_serial_id(const PifkApp* app) {
    /* Guard the persisted value: settings.json is user-editable. */
    return (app->gpio_serial_id == (uint8_t)FuriHalSerialIdLpuart) ? FuriHalSerialIdLpuart :
                                                                     FuriHalSerialIdUsart;
}

void pifk_gpio_pins(const PifkApp* app, int32_t* tx_pin, int32_t* rx_pin) {
    /* Ask the firmware for the header numbering rather than hardcoding
     * it: this cannot drift, and it stays correct if the numbering ever
     * differs across hardware revisions. */
    bool usart = (gpio_serial_id(app) == FuriHalSerialIdUsart);
    if(tx_pin) {
        *tx_pin = furi_hal_resources_get_ext_pin_number(usart ? &gpio_usart_tx : &gpio_ext_pb2);
    }
    if(rx_pin) {
        *rx_pin = furi_hal_resources_get_ext_pin_number(usart ? &gpio_usart_rx : &gpio_ext_pb3);
    }
}

/* ── Acquire / release ───────────────────────────────────────── */

static FuriHalSerialHandle* gpio_acquire(PifkApp* app) {
    FuriHalSerialHandle* h = furi_hal_serial_control_acquire(gpio_serial_id(app));
    if(!h) {
        gpio_error = (gpio_serial_id(app) == FuriHalSerialIdUsart) ?
                         "USART busy (CLI/logging). Try LPUART." :
                         "LPUART busy.";
        return NULL;
    }
    furi_hal_serial_init(h, app->gpio_baud);
    return h;
}

static void gpio_release(FuriHalSerialHandle* h) {
    if(!h) return;
    furi_hal_serial_deinit(h);
    furi_hal_serial_control_release(h);
}

/* ── Transmit ────────────────────────────────────────────────── */

static void gpio_send_line_ending(PifkApp* app, FuriHalSerialHandle* h) {
    if(app->gpio_line_ending == PifkGpioLineEndingCrLf) {
        furi_hal_serial_tx(h, (const uint8_t*)"\r\n", 2);
    } else if(app->gpio_line_ending == PifkGpioLineEndingLf) {
        furi_hal_serial_tx(h, (const uint8_t*)"\n", 1);
    }
}

/* Write `text` on an already-initialised handle. Returns false if an
 * abort cut it short, so callers can distinguish "sent" from "stopped". */
static bool gpio_write(PifkApp* app, FuriHalSerialHandle* h, const char* text) {
    size_t len = strlen(text);
    uint32_t delay = app->gpio_byte_delay_ms;
    if(delay > PIFK_GPIO_MAX_BYTE_DELAY_MS) delay = PIFK_GPIO_MAX_BYTE_DELAY_MS;

    if(delay == 0) {
        /* Burst: the driver buffers and returns before the wire is
         * clear, so wait before deinit or the tail is truncated. */
        furi_hal_serial_tx(h, (const uint8_t*)text, len);
    } else {
        for(size_t i = 0; i < len; i++) {
            if(!app->executing) return false; /* Stop pressed */
            furi_hal_serial_tx(h, (const uint8_t*)&text[i], 1);
            furi_hal_serial_tx_wait_complete(h);
            furi_delay_ms(delay);
        }
    }
    return true;
}

bool pifk_execute_gpio(PifkApp* app, const PifkPayload* payload) {
    gpio_error = NULL;
    if(!payload || !payload->text || !payload->text[0]) {
        gpio_error = "Payload is empty.";
        return false;
    }

    FuriHalSerialHandle* h = gpio_acquire(app);
    if(!h) return false;

    app->executing = true;
    bool complete = gpio_write(app, h, payload->text);
    if(complete) {
        gpio_send_line_ending(app, h);
    }
    furi_hal_serial_tx_wait_complete(h);
    app->executing = false;

    gpio_release(h);

    if(!complete) gpio_error = "Stopped before the payload finished.";
    return complete;
}

/* ── Response capture ────────────────────────────────────────── */

const char* pifk_gpio_response(size_t* len) {
    if(len) *len = gpio_resp.len;
    return gpio_resp.buf;
}

bool pifk_gpio_response_truncated(void) {
    return gpio_resp.overflow;
}

/* Interrupt context: read the byte and store it. Anything more
 * expensive than this belongs on the main thread. */
static void
    gpio_capture_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    UNUSED(ctx);
    if(event != FuriHalSerialRxEventData) return;
    uint8_t byte = furi_hal_serial_async_rx(handle);
    if(gpio_resp.len < PIFK_GPIO_RX_CAPACITY) {
        gpio_resp.buf[gpio_resp.len++] = (char)byte;
    } else {
        gpio_resp.overflow = true;
    }
}

bool pifk_execute_gpio_capture(PifkApp* app, const PifkPayload* payload, uint32_t listen_ms) {
    gpio_error = NULL;
    memset(&gpio_resp, 0, sizeof(gpio_resp));

    if(!payload || !payload->text || !payload->text[0]) {
        gpio_error = "Payload is empty.";
        return false;
    }
    if(listen_ms > PIFK_MAX_DELAY_MS) listen_ms = PIFK_MAX_DELAY_MS;

    FuriHalSerialHandle* h = gpio_acquire(app);
    if(!h) return false;

    /* Listen from before the send, so a target that replies immediately
     * is not missed while we are still transmitting. */
    furi_hal_serial_async_rx_start(h, gpio_capture_rx_cb, app, false);

    app->executing = true;
    bool complete = gpio_write(app, h, payload->text);
    if(complete) {
        gpio_send_line_ending(app, h);
    }
    furi_hal_serial_tx_wait_complete(h);

    /* Poll in slices so an abort during the listen window is honoured
     * rather than making the user wait out the whole timeout. */
    uint32_t waited = 0;
    while(waited < listen_ms && app->executing) {
        furi_delay_ms(10);
        waited += 10;
    }
    app->executing = false;

    furi_hal_serial_async_rx_stop(h);
    gpio_release(h);

    /* NUL-terminate for the text views; len is the authority on length
     * since a reply may legitimately contain NUL bytes. */
    gpio_resp.buf[gpio_resp.len] = '\0';

    if(!complete) {
        gpio_error = "Stopped before the payload finished.";
        return false;
    }
    return true;
}

/* ── Loopback self-test ──────────────────────────────────────── */

static void
    gpio_loopback_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    UNUSED(ctx);
    if(event != FuriHalSerialRxEventData) return;
    uint8_t byte = furi_hal_serial_async_rx(handle);
    if(gpio_rx.len < sizeof(gpio_rx.buf)) {
        gpio_rx.buf[gpio_rx.len++] = byte;
    }
}

bool pifk_gpio_loopback_test(PifkApp* app, size_t* sent, size_t* received) {
    gpio_error = NULL;
    if(sent) *sent = 0;
    if(received) *received = 0;

    FuriHalSerialHandle* h = gpio_acquire(app);
    if(!h) return false;

    memset(&gpio_rx, 0, sizeof(gpio_rx));
    furi_hal_serial_async_rx_start(h, gpio_loopback_rx_cb, app, false);

    const char* probe = GPIO_LOOPBACK_PROBE;
    size_t probe_len = strlen(probe);
    furi_hal_serial_tx(h, (const uint8_t*)probe, probe_len);
    furi_hal_serial_tx_wait_complete(h);

    /* Poll rather than block: the callback fires from an interrupt, so
     * there is nothing to wait on, and a short bounded wait is enough
     * for a wire loop at any supported baud. */
    uint32_t waited = 0;
    while(gpio_rx.len < probe_len && waited < GPIO_LOOPBACK_TIMEOUT_MS) {
        furi_delay_ms(10);
        waited += 10;
    }

    furi_hal_serial_async_rx_stop(h);
    gpio_release(h);

    size_t got = gpio_rx.len;
    if(sent) *sent = probe_len;
    if(received) *received = got;

    if(got == 0) {
        gpio_error = "Nothing received. Check the TX-RX jumper.";
        return false;
    }
    if(got != probe_len || memcmp(gpio_rx.buf, probe, probe_len) != 0) {
        gpio_error = "Data corrupted. Check baud rate and wiring.";
        return false;
    }
    return true;
}
