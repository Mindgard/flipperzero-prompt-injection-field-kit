/*
 * BadUSB (HID keyboard injection) execution engine.
 *
 * Types payload text character-by-character via USB HID,
 * then presses Enter.  For conversations, loops through
 * turns with configurable inter-turn delays.
 */

#include "badusb_exec.h"
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>

/* ── ASCII → HID keycode lookup ──────────────────────────────── */

uint16_t hid_ascii_to_key(char c) {
    /* Letters */
    if(c >= 'a' && c <= 'z') return HID_KEYBOARD_A + (c - 'a');
    if(c >= 'A' && c <= 'Z') return (HID_KEYBOARD_A + (c - 'A')) | KEY_MOD_LEFT_SHIFT;

    /* Digits */
    if(c == '0') return HID_KEYBOARD_0;
    if(c >= '1' && c <= '9') return HID_KEYBOARD_1 + (c - '1');

    /* Common punctuation */
    switch(c) {
    case ' ':
        return HID_KEYBOARD_SPACEBAR;
    case '\n':
        return HID_KEYBOARD_RETURN;
    case '\t':
        return HID_KEYBOARD_TAB;
    /* ESC is needed for printer command sequences such as Brother's
     * `ESC i a <n>` command-mode switch, which a barcode-scanner-shaped
     * HID device has to send as keystrokes. Without this the byte is
     * silently dropped and the sequence arrives malformed. */
    case '\x1b':
        return HID_KEYBOARD_ESCAPE;
    case '.':
        return HID_KEYBOARD_DOT;
    case ',':
        return HID_KEYBOARD_COMMA;
    case ';':
        return HID_KEYBOARD_SEMICOLON;
    case ':':
        return HID_KEYBOARD_SEMICOLON | KEY_MOD_LEFT_SHIFT;
    case '\'':
        return HID_KEYBOARD_APOSTROPHE;
    case '"':
        return HID_KEYBOARD_APOSTROPHE | KEY_MOD_LEFT_SHIFT;
    case '-':
        return HID_KEYBOARD_MINUS;
    case '_':
        return HID_KEYBOARD_MINUS | KEY_MOD_LEFT_SHIFT;
    case '=':
        return HID_KEYBOARD_EQUAL_SIGN;
    case '+':
        return HID_KEYBOARD_EQUAL_SIGN | KEY_MOD_LEFT_SHIFT;
    case '/':
        return HID_KEYBOARD_SLASH;
    case '?':
        return HID_KEYBOARD_SLASH | KEY_MOD_LEFT_SHIFT;
    case '\\':
        return HID_KEYBOARD_BACKSLASH;
    case '|':
        return HID_KEYBOARD_BACKSLASH | KEY_MOD_LEFT_SHIFT;
    case '[':
        return HID_KEYBOARD_OPEN_BRACKET;
    case '{':
        return HID_KEYBOARD_OPEN_BRACKET | KEY_MOD_LEFT_SHIFT;
    case ']':
        return HID_KEYBOARD_CLOSE_BRACKET;
    case '}':
        return HID_KEYBOARD_CLOSE_BRACKET | KEY_MOD_LEFT_SHIFT;
    case '`':
        return HID_KEYBOARD_GRAVE_ACCENT;
    case '~':
        return HID_KEYBOARD_GRAVE_ACCENT | KEY_MOD_LEFT_SHIFT;
    case '!':
        return HID_KEYBOARD_1 | KEY_MOD_LEFT_SHIFT;
    case '@':
        return HID_KEYBOARD_2 | KEY_MOD_LEFT_SHIFT;
    case '#':
        return HID_KEYBOARD_3 | KEY_MOD_LEFT_SHIFT;
    case '$':
        return HID_KEYBOARD_4 | KEY_MOD_LEFT_SHIFT;
    case '%':
        return HID_KEYBOARD_5 | KEY_MOD_LEFT_SHIFT;
    case '^':
        return HID_KEYBOARD_6 | KEY_MOD_LEFT_SHIFT;
    case '&':
        return HID_KEYBOARD_7 | KEY_MOD_LEFT_SHIFT;
    case '*':
        return HID_KEYBOARD_8 | KEY_MOD_LEFT_SHIFT;
    case '(':
        return HID_KEYBOARD_9 | KEY_MOD_LEFT_SHIFT;
    case ')':
        return HID_KEYBOARD_0 | KEY_MOD_LEFT_SHIFT;
    case '<':
        return HID_KEYBOARD_COMMA | KEY_MOD_LEFT_SHIFT;
    case '>':
        return HID_KEYBOARD_DOT | KEY_MOD_LEFT_SHIFT;
    default:
        return 0; /* unsupported character — skip */
    }
}

/* ── Host enumeration probe ──────────────────────────────────── */

bool pifk_badusb_probe_host(PifkApp* app, uint32_t timeout_ms, uint32_t* waited_ms) {
    UNUSED(app); /* kept for signature symmetry with the execute functions */
    if(waited_ms) *waited_ms = 0;
    if(furi_hal_usb_is_locked()) return false;

    FuriHalUsbInterface* prev_usb = furi_hal_usb_get_config();
    furi_hal_usb_set_config(&usb_hid, NULL);

    /* Poll rather than sleep for the whole timeout: an embedded host may
     * enumerate much faster or much slower than a desktop, and the
     * elapsed time is itself the useful datum. */
    uint32_t waited = 0;
    bool connected = false;
    while(waited < timeout_ms) {
        if(furi_hal_hid_is_connected()) {
            connected = true;
            break;
        }
        furi_delay_ms(50);
        waited += 50;
    }

    /* Always restore, even on failure: leaving the Flipper as a keyboard
     * after the probe would surprise whatever it is plugged into next. */
    furi_hal_usb_set_config(prev_usb, NULL);
    furi_delay_ms(200);

    if(waited_ms) *waited_ms = waited;
    return connected;
}

/* ── Payload compatibility check ─────────────────────────────── */

size_t pifk_badusb_count_unmappable(const char* text) {
    if(!text) return 0;
    size_t n = 0;
    for(size_t i = 0; text[i]; i++) {
        /* Bytes >= 0x80 are part of a multi-byte UTF-8 sequence, which
         * has no HID keycode at all; hid_ascii_to_key() returns 0 for
         * those and for unsupported ASCII alike. */
        if(hid_ascii_to_key(text[i]) == 0) n++;
    }
    return n;
}

/* ── Internal: type a single character ───────────────────────── */

static void type_char(char c) {
    uint16_t key = hid_ascii_to_key(c);
    if(key == 0) return; /* unsupported char */

    furi_hal_hid_kb_press(key);
    furi_delay_ms(10);
    furi_hal_hid_kb_release(key);
    furi_delay_ms(10);
}

/* ── Internal: type a string ─────────────────────────────────── */

static void type_string(const char* text) {
    for(size_t i = 0; text[i]; i++) {
        type_char(text[i]);
    }
}

/* ── Internal: press Enter ───────────────────────────────────── */

static void press_enter(void) {
    furi_hal_hid_kb_press(HID_KEYBOARD_RETURN);
    furi_delay_ms(10);
    furi_hal_hid_kb_release(HID_KEYBOARD_RETURN);
}

/* ── Public API ──────────────────────────────────────────────── */

bool pifk_execute_badusb(PifkApp* app, const PifkPayload* payload) {
    if(!payload || !payload->text || !payload->text[0]) return false;

    app->executing = true;

    /* Request HID USB profile */
    FuriHalUsbInterface* prev_usb = furi_hal_usb_get_config();
    furi_hal_usb_set_config(&usb_hid, NULL);
    furi_delay_ms(app->badusb_delay_ms);

    /* Verify HID is connected */
    if(!furi_hal_hid_is_connected()) {
        /* Restore previous USB profile */
        furi_hal_usb_set_config(prev_usb, NULL);
        app->executing = false;
        return false;
    }

    /* Type the payload text */
    type_string(payload->text);

    /* Press Enter to submit */
    press_enter();

    /* Small cooldown */
    furi_delay_ms(100);

    /* Restore previous USB profile */
    furi_hal_usb_set_config(prev_usb, NULL);
    furi_delay_ms(200);

    app->executing = false;
    return true;
}

void pifk_badusb_abort(PifkApp* app) {
    app->executing = false;
}
