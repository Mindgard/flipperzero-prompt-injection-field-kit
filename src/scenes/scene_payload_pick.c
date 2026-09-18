/*
 * Payload pick scene — choose a payload for the selected channel, and
 * execute it.
 *
 * ┌──────────────────────────┐
 * │ Show a QR - 18 of 45 fit │
 * │ > * qr-ignore            │
 * │   qr-system-leak         │
 * │   qr-tool-abuse          │
 * └──────────────────────────┘
 *
 * The list only contains payloads this channel can carry, which is the
 * whole reason the navigation was inverted: the four "too long" and "not
 * typable" rejection dialogs this replaces existed only because the
 * payload was chosen before the channel. A 128x64 screen has no room for
 * a validation layer, so the only affordable validation is not offering
 * the invalid option.
 *
 * The header names the count when anything was filtered out. An operator
 * not told that 27 of 45 payloads are missing will conclude the library
 * is small.
 *
 * Long-press OK stars a payload; see payload_picker.c.
 */

#include "../pifk_app.h"
#include "../channel/channel.h"
#include "payload_picker.h"
#include <furi_hal_bt.h>
#include "../execute/badusb_exec.h"
#include "../execute/nfc_exec.h"
#include "../execute/ble_exec.h"
#include "../execute/qr_exec.h"
#include "../execute/usb_descriptor_exec.h"
#include "../execute/gpio_exec.h"
#include "../execute/i2c_exec.h"
#include "../execute/ble_gatt_exec.h"
#include "../execute/nfc_listener_exec.h"
#include "../payload/capture_log.h"
#include <string.h>

/* Scene state: selected row, plus a flag for "a result view is showing"
 * so Back returns to the payload list rather than leaving the channel. */
#define STATE_IN_RESULT 0x8000u
#define STATE_ROW_MASK  0x7FFFu

/* Every result goes through a TextBox, which scrolls.
 *
 * These used to use DialogEx, which does not: it draws text at a fixed
 * offset and anything past the screen edge is simply not rendered, with
 * nothing to indicate it happened. The text below starts at y=16 on a
 * 64px screen, so about four lines fit — while the messages run to six,
 * seven, and in the GPIO "no reply" case twelve. The advice about
 * checking the wiring, which is the most useful text in the app, was
 * the part being cut off.
 *
 * The header is prepended as a line rather than passed to a dialog's
 * header slot, so it scrolls away with the rest instead of eating a
 * fixed band of a screen that has none to spare.
 */
static void payload_pick_show_result(PifkApp* app, const char* header, bool ok) {
    /* Build into a second buffer: text_buf already holds the body, and
     * prepending in place would need a memmove of up to 512 bytes. */
    static char framed[640];
    snprintf(framed, sizeof(framed), "%s\n\n%s", header, app->text_buf);

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, framed);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
    notification_message(app->notifications, ok ? &sequence_success : &sequence_error);
}

/* For content that is already framed, or is the target's own reply and
 * should not be dressed with a header. */
static void payload_pick_show_text(PifkApp* app, bool ok) {
    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, app->text_buf);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
    notification_message(app->notifications, ok ? &sequence_success : &sequence_error);
}

/* ── Per-channel execution ───────────────────────────────────── *
 *
 * Each returns having put something on screen. The eligibility filter
 * has already guaranteed the payload fits and is representable, so these
 * carry no capacity checks — that is the point of the inversion.
 */

static void exec_badusb(PifkApp* app, const PifkPayload* p) {
    notification_message(app->notifications, &sequence_blink_start_cyan);
    bool ok = pifk_execute_badusb(app, p);
    notification_message(app->notifications, &sequence_blink_stop);

    if(ok) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Typed %u characters as\n"
            "a USB keyboard.",
            (unsigned)strlen(p->text));
    } else {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Could not switch USB to\n"
            "HID. Another app may\n"
            "hold it.");
    }
    payload_pick_show_result(app, ok ? "Typed" : "BadUSB Error", ok);
}

static void exec_usbdesc(PifkApp* app, const PifkPayload* p) {
    bool ok = pifk_execute_usb_descriptor(app, p);

    if(ok) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Advertising as a USB\n"
            "device. Plug into the\n"
            "target to have it\n"
            "enumerate and log the\n"
            "payload.\n"
            "Back to restore USB.");
    } else {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Could not switch USB\n"
            "mode. Another app may\n"
            "hold it.");
    }
    payload_pick_show_result(app, ok ? "USB Descriptor" : "USB Error", ok);
}

static void exec_nfc_file(PifkApp* app, const PifkPayload* p) {
    /* Hands over to the built-in NFC app, which means this app exits —
     * so there is no result screen to show. */
    notification_message(app->notifications, &sequence_blink_start_cyan);
    bool ok = pifk_execute_nfc(app, p, true);
    notification_message(app->notifications, ok ? &sequence_success : &sequence_error);
    notification_message(app->notifications, &sequence_blink_stop);
}

static void exec_nfc_emulate(PifkApp* app, const PifkPayload* p, bool as_url) {
    notification_message(app->notifications, &sequence_blink_start_cyan);
    bool ok = pifk_execute_nfc_listener(app, p, as_url ? PifkNfcRecordUri : PifkNfcRecordText);

    if(ok && as_url) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Emulating NTAG215 with\n"
            "the payload as a URL.\n\n"
            "Tap a phone: iOS shows\n"
            "a banner and offers to\n"
            "open it. Back to stop.");
    } else if(ok) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Emulating NTAG215 with\n"
            "%u bytes of NDEF text.\n\n"
            "Read by a reader app or\n"
            "a kiosk. A stock phone\n"
            "reads but ignores text\n"
            "records. Back to stop.",
            (unsigned)strlen(p->text));
    } else if(as_url) {
        /* Percent-encoding costs up to three bytes a character, so a
         * payload that passed the text-record filter can still be too
         * long once encoded. */
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Could not emulate. The\n"
            "payload may be too long\n"
            "once URL-encoded, or the\n"
            "NFC hardware is in use.");
        notification_message(app->notifications, &sequence_blink_stop);
    } else {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Could not claim the NFC\n"
            "hardware.");
        notification_message(app->notifications, &sequence_blink_stop);
    }
    payload_pick_show_result(app, ok ? "NFC Emulating" : "NFC Error", ok);
}

static void exec_ble_beacon(PifkApp* app, const PifkPayload* p) {
    notification_message(app->notifications, &sequence_blink_start_blue);
    bool ok = pifk_execute_ble(app, p, 0);

    if(ok) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "%.28s\n"
            "Advertising 60s.\n"
            "Back to stop.",
            p->name);
    } else if(!furi_hal_bt_is_active()) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Bluetooth is off.\n\n"
            "Enable it in the\n"
            "Flipper's Settings >\n"
            "Bluetooth, then retry.");
    } else {
        snprintf(app->text_buf, sizeof(app->text_buf), "Failed to start\nBLE beacon.");
    }

    payload_pick_show_result(app, ok ? "BLE Active" : "BLE Error", ok);
    if(!ok) notification_message(app->notifications, &sequence_blink_stop);
}

static void exec_ble_gatt(PifkApp* app, const PifkPayload* p) {
    /* bt_profile_start() replaces the active profile, so the beacon
     * cannot be running alongside this. */
    if(app->ble_active) {
        pifk_ble_abort(app);
    }

    notification_message(app->notifications, &sequence_blink_start_blue);
    bool ok = pifk_execute_ble_gatt(app, p);

    if(ok) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Serving %u bytes over\n"
            "%u characteristic(s).\n\n"
            "Connect and read to\n"
            "collect. No pairing.\n"
            "Back to stop.",
            (unsigned)strlen(p->text),
            pifk_ble_gatt_chars_used(app));
    } else if(!furi_hal_bt_is_active()) {
        /* By far the most likely cause, and the one an operator can fix
         * without touching the app. */
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Bluetooth is off.\n\n"
            "Enable it in the\n"
            "Flipper's Settings >\n"
            "Bluetooth, then retry.");
    } else {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Could not start the\n"
            "GATT profile.");
    }

    payload_pick_show_result(app, ok ? "BLE GATT Active" : "BLE GATT Error", ok);
    if(!ok) notification_message(app->notifications, &sequence_blink_stop);
}

static void exec_qr(PifkApp* app, const PifkPayload* p) {
    /* Switches to its own view and stays there until Back. */
    if(!pifk_execute_qr(app, p)) {
        notification_message(app->notifications, &sequence_error);
    }
}

static void exec_gpio(PifkApp* app, const PifkPayload* p) {
    int32_t tx = -1, rx = -1;
    pifk_gpio_pins(app, &tx, &rx);

    notification_message(app->notifications, &sequence_blink_start_cyan);
    bool ok = pifk_execute_gpio(app, p);
    notification_message(app->notifications, &sequence_blink_stop);

    if(ok) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Sent %u bytes at %lu\n"
            "baud on pin %ld (TX).\n\n"
            "Ground must be shared\n"
            "with the target.",
            (unsigned)strlen(p->text),
            (unsigned long)app->gpio_baud,
            (long)tx);
    } else {
        const char* err = pifk_gpio_last_error();
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "%s\n\nSettings > GPIO to\nchange port or baud.",
            err ? err : "Send failed.");
    }
    payload_pick_show_result(app, ok ? "GPIO Sent" : "GPIO Error", ok);
}

static void exec_gpio_capture(PifkApp* app, const PifkPayload* p) {
    /* The only channel that can report whether the payload had an effect
     * rather than just that it was delivered — the reply is the
     * evidence. */
    notification_message(app->notifications, &sequence_blink_start_cyan);
    bool ok = pifk_execute_gpio_capture(app, p, app->gpio_listen_ms);
    notification_message(app->notifications, &sequence_blink_stop);

    size_t rx_len = 0;
    const char* rx = pifk_gpio_response(&rx_len);

    /* Persist before rendering. A reply that only ever existed in a
     * TextBox is not evidence — the operator had to photograph the
     * screen. Logged even when rx_len is 0: a silent target is a
     * result worth keeping. */
    bool logged = false;
    if(ok) {
        logged = capture_log_record(
            p->name, "gpio", strlen(p->text), rx, rx_len, pifk_gpio_response_truncated());
    }

    if(!ok) {
        const char* err = pifk_gpio_last_error();
        snprintf(app->text_buf, sizeof(app->text_buf), "%s", err ? err : "Send failed.");
    } else if(rx_len == 0) {
        /* Not an error: a silent target is a result. Name the likely
         * causes rather than leaving the operator guessing. */
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Sent %u bytes.\n\n"
            "No reply in %lums.\n\n"
            "Either the target sent\n"
            "nothing, or RX is not\n"
            "wired. The loopback\n"
            "test checks the\n"
            "wiring.%s",
            (unsigned)strlen(p->text),
            (unsigned long)app->gpio_listen_ms,
            logged ? "" : "\n\n! Capture log write\n  failed.");
    } else {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Sent %u B, got %u B%s\n"
            "%s"
            "--- reply ---\n%s",
            (unsigned)strlen(p->text),
            (unsigned)rx_len,
            pifk_gpio_response_truncated() ? " (truncated)" : "",
            logged ? "" : "! log write failed\n",
            rx);
    }
    payload_pick_show_text(app, ok);
}

static void exec_i2c(PifkApp* app, const PifkPayload* p) {
    notification_message(app->notifications, &sequence_blink_start_cyan);
    bool ok = pifk_i2c_execute(app, p);
    notification_message(app->notifications, &sequence_blink_stop);

    size_t written = pifk_i2c_bytes_written();

    if(ok) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Wrote %u bytes to\n"
            "0x%02X in %u-byte\n"
            "transactions.\n\n"
            "Ground must be shared\n"
            "with the target.",
            (unsigned)written,
            app->i2c_address,
            PIFK_I2C_CHUNK);
    } else {
        const char* err = pifk_i2c_last_error();
        /* A failed write may still have delivered bytes, and those
         * cannot be taken back. Say how many. */
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "%s\n\n"
            "%u bytes were written\n"
            "before this failed.\n\n"
            "Scan the bus, then set\n"
            "the address in\n"
            "Settings > I2C.",
            err ? err : "Write failed.",
            (unsigned)written);
    }
    payload_pick_show_result(app, ok ? "I2C Sent" : "I2C Error", ok);
}

/* ── Dispatch ────────────────────────────────────────────────── */

static void payload_pick_execute(PifkApp* app, const PifkPayload* p) {
    switch((PifkChannelId)app->selected_channel) {
    case PifkChannelBadUsb:
        exec_badusb(app, p);
        break;
    case PifkChannelUsbDesc:
        exec_usbdesc(app, p);
        break;
    case PifkChannelNfcFile:
        exec_nfc_file(app, p);
        break;
    case PifkChannelNfcText:
        exec_nfc_emulate(app, p, false);
        break;
    case PifkChannelNfcUrl:
        exec_nfc_emulate(app, p, true);
        break;
    case PifkChannelBleBeacon:
        exec_ble_beacon(app, p);
        break;
    case PifkChannelBleGatt:
        exec_ble_gatt(app, p);
        break;
    case PifkChannelQr:
        exec_qr(app, p);
        break;
    case PifkChannelGpio:
        exec_gpio(app, p);
        break;
    case PifkChannelGpioCapture:
        exec_gpio_capture(app, p);
        break;
    case PifkChannelI2cWrite:
        exec_i2c(app, p);
        break;

    /* Named rather than defaulted so adding a channel is a compile
     * error here until it is wired up. */
    case PifkChannelReserved3:
    case PifkChannelIdCount:
        break;
    }
}

static void payload_pick_callback(void* context, uint32_t index) {
    PifkApp* app = context;
    if(index >= app->payload_db->payload_count) return;

    app->selected_payload_index = (uint16_t)index;
    scene_manager_set_scene_state(
        app->scene_manager, PifkScenePayloadPick, index | STATE_IN_RESULT);

    payload_pick_execute(app, &app->payload_db->payloads[index]);
}

void pifk_scene_payload_pick_on_enter(void* context) {
    PifkApp* app = context;
    const PifkChannel* ch = pifk_channel_by_id(app->selected_channel);

    pifk_menu_reset(app->menu);

    /* Header must outlive this call: the menu stores the
     * pointer rather than copying. */
    static char header[48];
    pifk_payload_picker_header(app, ch, header, sizeof(header));
    pifk_menu_set_header(app->menu, header);

    uint16_t rows = pifk_payload_picker_build(app, ch, payload_pick_callback);

    if(rows == 0) {
        /* No payload fits. Only reachable by loading a payloads.json
         * whose entries all exceed this channel — but an empty submenu
         * gives no feedback at all, so say what happened. */
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "No loaded payload fits\n"
            "this channel's %u-byte\n"
            "limit.\n\n"
            "Try another channel, or\n"
            "add a shorter payload\n"
            "to payloads.json.",
            (unsigned)(ch ? ch->max_bytes : 0));
        payload_pick_show_result(app, "Nothing fits", false);
        return;
    }

    uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkScenePayloadPick);
    pifk_menu_set_selected_row(app->menu, (uint16_t)(state & STATE_ROW_MASK));

    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
}

bool pifk_scene_payload_pick_on_event(void* context, SceneManagerEvent event) {
    PifkApp* app = context;

    if(event.type == SceneManagerEventTypeBack) {
        uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkScenePayloadPick);
        if(state & STATE_IN_RESULT) {
            /* Stop whatever is still running, so the operator cannot
             * leave a beacon advertising or a tag emulating with nothing
             * on screen saying so.  Each stop is a no-op if that channel
             * was never started, so this needs no per-channel branch —
             * persists_after_exec documents which ones matter. */
            if(app->ble_active) {
                pifk_ble_abort(app);
            }
            pifk_usb_descriptor_stop(app);
            pifk_ble_gatt_stop(app);
            pifk_nfc_listener_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);

            /* The submenu still holds its items, so returning to it is a
             * view switch rather than a scene rebuild.  Going through
             * the scene manager here would tear down and re-enter the
             * scene, which is what on_exit is for. */
            scene_manager_set_scene_state(
                app->scene_manager, PifkScenePayloadPick, state & STATE_ROW_MASK);
            view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
            return true;
        }
    }
    return false;
}

void pifk_scene_payload_pick_on_exit(void* context) {
    PifkApp* app = context;

    /* Every channel that keeps running past its execute call is stopped
     * here. The table's persists_after_exec flag records which those
     * are; the stops themselves are unconditional because each is a
     * no-op when the channel was not started. */
    if(app->ble_active) {
        pifk_ble_abort(app);
    }
    pifk_usb_descriptor_stop(app);
    pifk_ble_gatt_stop(app);
    pifk_nfc_listener_stop(app);

    notification_message(app->notifications, &sequence_blink_stop);

    pifk_menu_reset(app->menu);
    text_box_reset(app->text_box);
}
