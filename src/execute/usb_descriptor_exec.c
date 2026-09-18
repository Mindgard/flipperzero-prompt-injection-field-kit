/*
 * USB descriptor injection.
 *
 * Every other channel in this kit needs the target to do something: put
 * a cursor in a text field, point a camera, tap a tag, pair a radio,
 * wire a board. This one needs the cable pushed in.
 *
 * When a USB device is attached, the host reads its identifying strings
 * as part of enumeration and writes them to logs before any driver
 * loads. Put payload text in those strings and it lands in the host's
 * records automatically. The interesting consumer is not the endpoint
 * user but whatever reads those logs afterwards — increasingly an
 * LLM-assisted triage or summarisation step.
 *
 * Implementation note: rather than writing a USB class from scratch, we
 * copy the CDC interface descriptor set by value and override only the
 * three string pointers and the device descriptor. The class's init,
 * deinit and configuration descriptor are reused verbatim, so the
 * device still enumerates as a well-formed serial port — it just has an
 * unusual name. This matters: a malformed configuration descriptor gets
 * the device rejected before the host reads any strings, which would
 * defeat the whole point.
 */

#include "usb_descriptor_exec.h"
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <usb_std.h>
#include <string.h>

/* A string descriptor sized for our maximum. The libusb_stm32 struct
 * uses a flexible array member, so we need a concrete equivalent. */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wString[PIFK_USBDESC_MAX_CHARS];
} PifkUsbString;

/* Descriptor storage must outlive the call: the USB stack keeps these
 * pointers and reads them when the host asks, which is after we return. */
static PifkUsbString pifk_str_manuf;
static PifkUsbString pifk_str_prod;
static PifkUsbString pifk_str_serial;
static struct usb_device_descriptor pifk_dev_descr;
static FuriHalUsbInterface pifk_usb_iface;

static FuriHalUsbInterface* pifk_usb_prev = NULL;
static bool pifk_usb_active = false;

/* ── String descriptor construction ──────────────────────────── */

/* Copy up to PIFK_USBDESC_MAX_CHARS characters of `src` into a
 * UTF-16 string descriptor. Returns the number of source characters
 * consumed so the caller can continue from there.
 *
 * ASCII only: a byte >= 0x80 is part of a UTF-8 sequence that would
 * need decoding to produce the right code unit, and emitting the raw
 * byte would corrupt the string. Substituting '?' keeps the descriptor
 * well-formed and keeps the character count honest. */
static size_t usbdesc_fill(PifkUsbString* dst, const char* src) {
    size_t i = 0;
    while(src[i] != '\0' && i < PIFK_USBDESC_MAX_CHARS) {
        unsigned char c = (unsigned char)src[i];
        dst->wString[i] = (c < 0x80) ? (uint16_t)c : (uint16_t)'?';
        i++;
    }
    /* bLength counts the two header bytes plus two bytes per character. */
    dst->bLength = (uint8_t)(2 + (i * 2));
    dst->bDescriptorType = USB_DTYPE_STRING;
    return i;
}

/* Blank a descriptor so an unused string is still valid if requested. */
static void usbdesc_clear(PifkUsbString* dst) {
    dst->bLength = 2;
    dst->bDescriptorType = USB_DTYPE_STRING;
    memset(dst->wString, 0, sizeof(dst->wString));
}

size_t pifk_usb_descriptor_lossy_chars(const char* text) {
    if(!text) return 0;
    size_t lossy = 0;
    size_t len = 0;
    for(size_t i = 0; text[i]; i++) {
        len++;
        if((unsigned char)text[i] >= 0x80) lossy++;
    }
    /* Anything beyond the three-string ceiling is dropped entirely. */
    if(len > PIFK_USBDESC_TOTAL_CHARS) {
        lossy += len - PIFK_USBDESC_TOTAL_CHARS;
    }
    return lossy;
}

/* ── Public API ──────────────────────────────────────────────── */

bool pifk_execute_usb_descriptor(PifkApp* app, const PifkPayload* payload) {
    if(!payload || !payload->text || !payload->text[0]) return false;
    if(furi_hal_usb_is_locked()) return false;

    /* Re-entry: drop the previous session before starting another, so
     * pifk_usb_prev always refers to a real prior configuration
     * rather than to our own interface. */
    if(pifk_usb_active) {
        pifk_usb_descriptor_stop(app);
    }

    /* Split the payload across the three strings in order. Product goes
     * first because it is the string most consistently logged and
     * displayed; manufacturer and serial take the remainder. */
    const char* cursor = payload->text;
    cursor += usbdesc_fill(&pifk_str_prod, cursor);
    if(*cursor) {
        cursor += usbdesc_fill(&pifk_str_manuf, cursor);
    } else {
        usbdesc_clear(&pifk_str_manuf);
    }
    if(*cursor) {
        usbdesc_fill(&pifk_str_serial, cursor);
    } else {
        usbdesc_clear(&pifk_str_serial);
    }

    /* Start from the CDC interface so init/deinit and the configuration
     * descriptor are a known-good serial device, then relabel it. */
    pifk_usb_iface = usb_cdc_single;

    /* Copy CDC's device descriptor and keep its class/protocol fields:
     * changing those without matching the configuration descriptor is
     * what gets a device rejected. Only the identity is ours. */
    if(usb_cdc_single.dev_descr) {
        memcpy(&pifk_dev_descr, usb_cdc_single.dev_descr, sizeof(pifk_dev_descr));
    }
    pifk_dev_descr.iManufacturer = 1;
    pifk_dev_descr.iProduct = 2;
    pifk_dev_descr.iSerialNumber = 3;

    pifk_usb_iface.dev_descr = &pifk_dev_descr;
    pifk_usb_iface.str_manuf_descr = &pifk_str_manuf;
    pifk_usb_iface.str_prod_descr = &pifk_str_prod;
    pifk_usb_iface.str_serial_descr = &pifk_str_serial;

    /* Remember what to go back to. Do this before switching, and only
     * if we are not already the active interface. */
    FuriHalUsbInterface* current = furi_hal_usb_get_config();
    if(current != &pifk_usb_iface) {
        pifk_usb_prev = current;
    }

    /* Switching configuration forces the host to re-enumerate, which is
     * what makes it read — and log — the new strings. */
    if(!furi_hal_usb_set_config(&pifk_usb_iface, NULL)) {
        return false;
    }

    pifk_usb_active = true;
    return true;
}

void pifk_usb_descriptor_stop(PifkApp* app) {
    UNUSED(app);
    if(!pifk_usb_active) return;

    pifk_usb_active = false;

    /* Leaving the Flipper advertising a hostile product string after the
     * scene closes would be a bug, not a feature. */
    if(pifk_usb_prev) {
        furi_hal_usb_set_config(pifk_usb_prev, NULL);
        pifk_usb_prev = NULL;
    }
}
