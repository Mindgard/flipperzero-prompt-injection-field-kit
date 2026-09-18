/*
 * Serial bridge protocol — remote control from a host over USB serial.
 *
 * Line-based text protocol over USB CDC channel 1 (115200 baud).
 *
 * This file runs on its own thread and deliberately owns very little.
 * It parses a command line, then hands anything that executes, mutates
 * the databases or draws to the screen over to the main thread (see
 * pifk_service_bridge_request) and waits for the result.  The
 * ViewDispatcher and the payload arrays belong to the main thread; the
 * GUI holds raw pointers into those arrays while it renders, so doing
 * the work here would race it.
 *
 * Commands are matched case-insensitively.  Longer prefixes are tested
 * before their shorter counterparts, so "EXEC NFCEMUURL" is not
 * swallowed by "EXEC NFCEMU", nor "STOP BLEGATT" by "STOP BLE".
 *
 *   Host → Flipper:
 *     PING                          Heartbeat
 *     STATUS                        Bridge state and record counts
 *     LIST                          List loaded payloads
 *     EXEC BADUSB <payload_name>    Type the payload over USB HID
 *     EXEC NFC <payload_name>       Write an .nfc NDEF file to the SD card
 *     EXEC NFCEMU <payload_name>    Emulate an NDEF text tag in-process
 *     EXEC NFCEMUURL <name>        Emulate it as a URL (what iOS surfaces)
 *     EXEC BLE <payload_name>       Start extra-beacon advertising
 *     EXEC BLEGATT <payload_name>   Serve payload from readable GATT chars
 *     EXEC QR <payload_name>        Show the payload as a QR code
 *     EXEC USBDESC <payload_name>   Advertise payload in USB ID strings
 *     EXEC GPIO <payload_name>      Send payload as bytes on the UART TX pin
 *     EXEC GPIOCAP <payload_name>   Send, then capture the target's reply
 *     EXEC I2C <payload_name>       Write payload to the configured I2C address
 *     SCAN I2C                      Probe 0x08-0x77, list responding addresses
 *     STOP                          Abort the running execution
 *     STOP BLE                      Stop an active BLE broadcast
 *     STOP BLEGATT                  Stop serving GATT, restore default profile
 *     STOP NFCEMU                   Stop tag emulation, release NFC
 *     STOP USBDESC                  Restore the previous USB config
 *     LOAD <json>                   Push one payload: {"name":..,"text":..}
 *     SET DELAY <ms>                Delay before typing starts
 *     RELOAD                        Re-read everything from the SD card
 *
 *   Flipper → Host, one line per command:
 *     OK                            STOP / STOP BLE / STOP USBDESC
 *     OK PONG                       PING
 *     OK STATUS <state> payloads=<n>
 *     OK DONE <name>                EXEC BADUSB finished
 *     OK WRITTEN <path>             EXEC NFC
 *     OK BROADCASTING <name>        EXEC BLE
 *     OK SERVING <name>             EXEC BLEGATT
 *     OK EMULATING <name>           EXEC NFCEMU
 *     OK DISPLAYING <name>          EXEC QR
 *     OK ADVERTISING <name>         EXEC USBDESC (append: chars dropped)
 *     OK SENT <name>                EXEC GPIO
 *     OK REPLY <name>: <text>       EXEC GPIOCAP (or '(no reply)')
 *     OK I2CWROTE <name>: <n> bytes to 0x<addr>
 *     OK DEVICES <n>: 0x.. 0x..     SCAN I2C
 *     OK loaded <name>              LOAD
 *     OK RELOADED payloads=<n>
 *     DATA [ ... ]                  LIST
 *     ERR <message>                 Anything that failed
 *
 * EXEC commands are synchronous: the reply arrives once the execution
 * has finished, so there is no separate "started" message.  BadUSB
 * execution reconfigures USB, which drops this CDC link for its
 * duration; the host should expect the reply to arrive after the link
 * comes back.
 */

#include "bridge_protocol.h"
#include "../payload/payload_db.h"
#include "../execute/badusb_exec.h"
#include "../execute/nfc_exec.h"
#include "../execute/ble_exec.h"
#include "../execute/qr_exec.h"
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ── Helpers ─────────────────────────────────────────────────── */

static void trim_crlf(char* s) {
    size_t len = strlen(s);
    while(len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

/* Case-insensitive prefix match. */
static bool starts_with(const char* str, const char* prefix) {
    while(*prefix) {
        if((*str | 0x20) != (*prefix | 0x20)) return false;
        str++;
        prefix++;
    }
    return true;
}

/* ── CDC channel ─────────────────────────────────────────────── */

#define BRIDGE_CDC_CH 1 /* Channel 0 = Flipper CLI, Channel 1 = bridge */

/* ── CDC RX callback — called from USB IRQ context ───────────── */

static void bridge_cdc_rx_callback(void* context) {
    PifkApp* app = context;
    uint8_t buf[CDC_DATA_SZ];
    int32_t len = furi_hal_cdc_receive(BRIDGE_CDC_CH, buf, sizeof(buf));
    if(len > 0) {
        furi_stream_buffer_send(app->serial_rx, buf, (size_t)len, 0);
    }
}

static CdcCallbacks bridge_cdc_callbacks = {
    .tx_ep_callback = NULL,
    .rx_ep_callback = bridge_cdc_rx_callback,
    .state_callback = NULL,
    .ctrl_line_callback = NULL,
    .config_callback = NULL,
};

/* ── Send response to host ───────────────────────────────────── */

void bridge_send(PifkApp* app, const char* response) {
    UNUSED(app);
    size_t len = strlen(response);

    /* Build contiguous buffer with CRLF appended so the line
     * terminator is never split from the body across USB packets. */
    size_t total = len + 2; /* +CRLF */
    uint8_t* buf = malloc(total);
    if(!buf) return;
    memcpy(buf, response, len);
    buf[len] = '\r';
    buf[len + 1] = '\n';

    /* Send in CDC_DATA_SZ chunks */
    const uint8_t* ptr = buf;
    while(total > 0) {
        uint16_t chunk = (total > CDC_DATA_SZ) ? CDC_DATA_SZ : (uint16_t)total;
        furi_hal_cdc_send(BRIDGE_CDC_CH, (uint8_t*)ptr, chunk);
        ptr += chunk;
        total -= chunk;
    }

    free(buf);
}

/* ── Deferred requests to the main thread ────────────────────── *
 *
 * The ViewDispatcher and the payload/conversation databases belong to
 * the main thread.  Touching either from here would race the GUI (which
 * holds raw pointers into the payload array while rendering), so every
 * command that executes, mutates or draws is packaged up and handed
 * over via a custom event.  We then block until the main thread
 * signals completion, which also serialises execution requests.
 */

#define BRIDGE_REQUEST_TIMEOUT_MS 120000

static bool
    bridge_dispatch(PifkApp* app, PifkRequestKind kind, const char* name, const char* text) {
    PifkRequest* req = &app->bridge_request;
    if(!req->done) return false; /* bridge not started */

    req->kind = kind;
    req->ok = false;
    req->detail[0] = '\0';
    strlcpy(req->name, name ? name : "", sizeof(req->name));
    strlcpy(req->text, text ? text : "", sizeof(req->text));

    furi_event_flag_clear(req->done, PIFK_REQUEST_DONE_FLAG);

    view_dispatcher_send_custom_event(app->view_dispatcher, PIFK_EVENT_BRIDGE_REQUEST);

    uint32_t flags = furi_event_flag_wait(
        req->done, PIFK_REQUEST_DONE_FLAG, FuriFlagWaitAny, BRIDGE_REQUEST_TIMEOUT_MS);

    /* furi_event_flag_wait returns an error bitmask on timeout. */
    return (flags & PIFK_REQUEST_DONE_FLAG) != 0;
}

/* Run a request and report the outcome using the given verb. */
static void bridge_dispatch_reply(
    PifkApp* app,
    PifkRequestKind kind,
    const char* name,
    const char* text,
    const char* ok_verb) {
    char buf[192];

    if(!bridge_dispatch(app, kind, name, text)) {
        bridge_send(app, "ERR request timed out");
        return;
    }

    const PifkRequest* req = &app->bridge_request;
    if(req->ok) {
        /* ok_verb is empty for commands with nothing to report (STOP),
         * which reply with a bare "OK". */
        if(!ok_verb[0]) {
            snprintf(buf, sizeof(buf), "OK");
        } else if(req->detail[0]) {
            snprintf(buf, sizeof(buf), "OK %s %s", ok_verb, req->detail);
        } else {
            snprintf(buf, sizeof(buf), "OK %s %.100s", ok_verb, name ? name : "");
        }
    } else if(req->detail[0]) {
        snprintf(buf, sizeof(buf), "ERR %s", req->detail);
    } else if(ok_verb[0]) {
        snprintf(buf, sizeof(buf), "ERR %s failed: %.80s", ok_verb, name ? name : "");
    } else {
        snprintf(buf, sizeof(buf), "ERR command failed");
    }
    bridge_send(app, buf);
}

/* ── Command handlers ────────────────────────────────────────── */

static void handle_ping(PifkApp* app) {
    bridge_send(app, "OK PONG");
}

static void handle_status(PifkApp* app) {
    const char* state;
    switch(app->bridge_state) {
    case PifkBridgeIdle:
        state = "idle";
        break;
    case PifkBridgeListening:
        state = "listening";
        break;
    case PifkBridgeConnected:
        state = "connected";
        break;
    case PifkBridgeExecuting:
        state = "executing";
        break;
    case PifkBridgeError:
        state = "error";
        break;
    default:
        state = "unknown";
        break;
    }

    char buf[128];
    furi_mutex_acquire(app->db_mutex, FuriWaitForever);
    snprintf(buf, sizeof(buf), "OK STATUS %s payloads=%u", state, app->payload_db->payload_count);
    furi_mutex_release(app->db_mutex);
    bridge_send(app, buf);
}

#define LIST_BUF_SIZE 4096

/* Append to a bounded buffer, tracking the write cursor.  snprintf
 * returns the length it *would* have written, so the cursor has to be
 * clamped or it runs past the end of the buffer. */
static bool list_append(char* out, size_t cap, size_t* pos, const char* fmt, ...) {
    if(*pos >= cap) return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if(n < 0) return false;
    if((size_t)n >= cap - *pos) {
        *pos = cap; /* truncated */
        return false;
    }
    *pos += (size_t)n;
    return true;
}

static void handle_list(PifkApp* app) {
    /* Build the entire JSON array as one string and send it in a single
     * bridge_send() call to avoid USB packet boundary issues. */
    char* out = malloc(LIST_BUF_SIZE);
    if(!out) {
        bridge_send(app, "ERR out of memory");
        return;
    }

    size_t pos = 0;
    bool truncated = false;
    list_append(out, LIST_BUF_SIZE, &pos, "DATA [");

    furi_mutex_acquire(app->db_mutex, FuriWaitForever);
    for(uint16_t i = 0; i < app->payload_db->payload_count; i++) {
        const PifkPayload* p = &app->payload_db->payloads[i];
        if(!list_append(
               out,
               LIST_BUF_SIZE,
               &pos,
               "%s{\"name\":\"%s\",\"category\":\"%s\",\"builtin\":%s}",
               (i > 0) ? "," : "",
               p->name,
               p->category,
               p->is_builtin ? "true" : "false")) {
            truncated = true;
            break;
        }
    }
    furi_mutex_release(app->db_mutex);

    if(truncated) {
        /* Report explicitly rather than emitting a silently short list. */
        bridge_send(app, "ERR payload list too large for response buffer");
    } else {
        list_append(out, LIST_BUF_SIZE, &pos, "]");
        bridge_send(app, out);
    }
    free(out);
}

/* Extract a quoted string value for `key` from a flat JSON object,
 * honouring the standard escape sequences.  Returns false if the key
 * is absent or malformed. */
static bool json_field(const char* json, const char* key, char* dst, size_t dst_len) {
    if(!dst || dst_len == 0) return false;
    dst[0] = '\0';

    char pattern[32];
    int pn = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if(pn < 0 || (size_t)pn >= sizeof(pattern)) return false;

    const char* k = strstr(json, pattern);
    if(!k) return false;

    const char* val = k + pn;
    while(*val && *val != ':')
        val++;
    if(*val != ':') return false;
    val++;
    while(*val == ' ' || *val == '\t')
        val++;
    if(*val != '"') return false;
    val++;

    size_t i = 0;
    while(*val && *val != '"' && i < dst_len - 1) {
        if(*val == '\\' && *(val + 1)) {
            val++;
            switch(*val) {
            case 'n':
                dst[i++] = '\n';
                break;
            case 't':
                dst[i++] = '\t';
                break;
            case 'r':
                dst[i++] = '\r';
                break;
            case '"':
                dst[i++] = '"';
                break;
            case '\\':
                dst[i++] = '\\';
                break;
            default:
                dst[i++] = *val;
                break;
            }
        } else {
            dst[i++] = *val;
        }
        val++;
    }
    dst[i] = '\0';
    return i > 0;
}

static void handle_load(PifkApp* app, const char* json) {
    const char* p = json;
    while(*p && *p != '{')
        p++;
    if(!*p) {
        bridge_send(app, "ERR invalid JSON");
        return;
    }

    char name[PIFK_MAX_NAME_LEN];
    char text[PIFK_MAX_TEXT_LEN];

    if(!json_field(p, "name", name, sizeof(name)) || !json_field(p, "text", text, sizeof(text))) {
        bridge_send(app, "ERR missing name or text in JSON");
        return;
    }

    bridge_dispatch_reply(app, PifkReqLoad, name, text, "loaded");
}

static void handle_set_delay(PifkApp* app, const char* value) {
    int32_t ms = atoi(value);
    if(ms < 0 || ms > PIFK_MAX_DELAY_MS) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ERR delay must be 0-%d ms", PIFK_MAX_DELAY_MS);
        bridge_send(app, buf);
        return;
    }
    app->badusb_delay_ms = (uint32_t)ms;
    char buf[64];
    snprintf(buf, sizeof(buf), "OK delay=%lu", (unsigned long)app->badusb_delay_ms);
    bridge_send(app, buf);
}

/* ── Command dispatcher ──────────────────────────────────────── */

void bridge_handle_command(PifkApp* app, const char* cmd) {
    /* Skip leading whitespace */
    while(*cmd == ' ' || *cmd == '\t')
        cmd++;

    if(starts_with(cmd, "PING")) {
        handle_ping(app);
    } else if(starts_with(cmd, "STATUS")) {
        handle_status(app);
    } else if(starts_with(cmd, "LIST")) {
        handle_list(app);
    } else if(starts_with(cmd, "EXEC BADUSB ")) {
        bridge_dispatch_reply(app, PifkReqExecBadUsb, cmd + 12, NULL, "DONE");
    } else if(starts_with(cmd, "EXEC NFCEMUURL ")) {
        bridge_dispatch_reply(app, PifkReqExecNfcEmuUrl, cmd + 15, NULL, "EMULATING");
    } else if(starts_with(cmd, "EXEC NFCEMU ")) {
        bridge_dispatch_reply(app, PifkReqExecNfcEmu, cmd + 12, NULL, "EMULATING");
    } else if(starts_with(cmd, "EXEC NFC ")) {
        bridge_dispatch_reply(app, PifkReqExecNfc, cmd + 9, NULL, "WRITTEN");
    } else if(starts_with(cmd, "EXEC BLEGATT ")) {
        bridge_dispatch_reply(app, PifkReqExecBleGatt, cmd + 13, NULL, "SERVING");
    } else if(starts_with(cmd, "EXEC BLE ")) {
        bridge_dispatch_reply(app, PifkReqExecBle, cmd + 9, NULL, "BROADCASTING");
    } else if(starts_with(cmd, "EXEC QR ")) {
        bridge_dispatch_reply(app, PifkReqExecQr, cmd + 8, NULL, "DISPLAYING");
    } else if(starts_with(cmd, "EXEC USBDESC ")) {
        bridge_dispatch_reply(app, PifkReqExecUsbDesc, cmd + 13, NULL, "ADVERTISING");
    } else if(starts_with(cmd, "EXEC GPIOCAP ")) {
        bridge_dispatch_reply(app, PifkReqExecGpioCapture, cmd + 13, NULL, "REPLY");
    } else if(starts_with(cmd, "EXEC GPIO ")) {
        bridge_dispatch_reply(app, PifkReqExecGpio, cmd + 10, NULL, "SENT");
    } else if(starts_with(cmd, "EXEC I2C ")) {
        /* Not "WRITTEN": EXEC NFC already uses that verb for a file path,
         * and a host keying on the verb would conflate the two. */
        bridge_dispatch_reply(app, PifkReqExecI2c, cmd + 9, NULL, "I2CWROTE");
    } else if(starts_with(cmd, "SCAN I2C")) {
        bridge_dispatch_reply(app, PifkReqScanI2c, NULL, NULL, "DEVICES");
    } else if(starts_with(cmd, "STOP USBDESC")) {
        bridge_dispatch_reply(app, PifkReqStopUsbDesc, NULL, NULL, "");
    } else if(starts_with(cmd, "STOP NFCEMU")) {
        bridge_dispatch_reply(app, PifkReqStopNfcEmu, NULL, NULL, "");
    } else if(starts_with(cmd, "STOP BLEGATT")) {
        bridge_dispatch_reply(app, PifkReqStopBleGatt, NULL, NULL, "");
    } else if(starts_with(cmd, "STOP BLE")) {
        bridge_dispatch_reply(app, PifkReqStopBle, NULL, NULL, "");
    } else if(starts_with(cmd, "STOP")) {
        bridge_dispatch_reply(app, PifkReqStopExec, NULL, NULL, "");
    } else if(starts_with(cmd, "LOAD ")) {
        handle_load(app, cmd + 5);
    } else if(starts_with(cmd, "SET DELAY ")) {
        handle_set_delay(app, cmd + 10);
    } else if(starts_with(cmd, "RELOAD")) {
        bridge_dispatch_reply(app, PifkReqReload, NULL, NULL, "RELOADED");
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "ERR unknown command: %.64s", cmd);
        bridge_send(app, buf);
    }
}

/* ── Bridge thread ───────────────────────────────────────────── */

static int32_t bridge_thread_entry(void* context) {
    PifkApp* app = context;
    app->bridge_state = PifkBridgeListening;

    char line_buf[PIFK_SERIAL_BUF_SIZE];
    size_t line_pos = 0;
    bool overflow = false;

    while(!(furi_thread_flags_get() & BRIDGE_THREAD_FLAG_STOP)) {
        uint8_t byte;
        size_t received = furi_stream_buffer_receive(app->serial_rx, &byte, 1, 100);

        if(received == 0) continue;

        if(byte == '\n' || byte == '\r') {
            if(overflow) {
                /* Reject the whole line rather than acting on a
                 * silently truncated command. */
                bridge_send(app, "ERR command too long");
                overflow = false;
                line_pos = 0;
            } else if(line_pos > 0) {
                line_buf[line_pos] = '\0';
                trim_crlf(line_buf);

                if(app->bridge_state == PifkBridgeListening) {
                    app->bridge_state = PifkBridgeConnected;
                }

                bridge_handle_command(app, line_buf);
                line_pos = 0;
            }
        } else if(line_pos < PIFK_SERIAL_BUF_SIZE - 1) {
            line_buf[line_pos++] = (char)byte;
        } else {
            overflow = true;
        }
    }

    app->bridge_state = PifkBridgeIdle;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

void bridge_start(PifkApp* app) {
    if(app->bridge_thread) return; /* already running */

    /* Event flag the bridge thread blocks on while the main thread
     * services a request. */
    app->bridge_request.done = furi_event_flag_alloc();
    app->bridge_request.kind = PifkReqNone;

    /* Switch USB to dual-CDC mode so channel 1 is available.
     * Channel 0 remains the Flipper CLI. */
    furi_hal_usb_unlock();
    furi_hal_usb_set_config(&usb_cdc_dual, NULL);
    furi_delay_ms(100); /* let USB re-enumerate */

    /* Register our RX callback on CDC channel 1 */
    furi_hal_cdc_set_callbacks(BRIDGE_CDC_CH, &bridge_cdc_callbacks, app);

    /* Drain any stale data in the stream buffer */
    furi_stream_buffer_reset(app->serial_rx);

    app->bridge_thread = furi_thread_alloc_ex("PifkBridge", 2048, bridge_thread_entry, app);
    furi_thread_start(app->bridge_thread);
}

void bridge_stop(PifkApp* app) {
    if(!app->bridge_thread) return;

    furi_thread_flags_set(furi_thread_get_id(app->bridge_thread), BRIDGE_THREAD_FLAG_STOP);

    /* We are on the main thread, which is the only thread that services
     * requests.  If the bridge is currently blocked waiting for one,
     * joining without releasing it would deadlock: release it first and
     * let bridge_dispatch() observe the stop flag. */
    if(app->bridge_request.done) {
        furi_event_flag_set(app->bridge_request.done, PIFK_REQUEST_DONE_FLAG);
    }

    furi_thread_join(app->bridge_thread);
    furi_thread_free(app->bridge_thread);
    app->bridge_thread = NULL;
    app->bridge_state = PifkBridgeIdle;

    /* Unregister CDC callbacks first so no further RX lands, then
     * restore single-CDC mode. */
    furi_hal_cdc_set_callbacks(BRIDGE_CDC_CH, NULL, NULL);
    furi_hal_usb_set_config(&usb_cdc_single, NULL);

    /* The thread has joined, so nothing can be waiting on this. */
    if(app->bridge_request.done) {
        furi_event_flag_free(app->bridge_request.done);
        app->bridge_request.done = NULL;
    }
    app->bridge_request.kind = PifkReqNone;
}
