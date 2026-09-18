#pragma once

#include "../pifk_app.h"

/* Present the Flipper as a USB device whose identifying strings carry
 * the payload, so a host logs the text during enumeration.
 *
 * Hosts record manufacturer, product and serial strings automatically
 * when a device is attached — Windows in EID 6416 (Security log) and
 * setupapi.dev.log, Linux via udev/dmesg, macOS in unified logging.
 * Nothing on the target has to be focused, scanned, tapped or paired;
 * the payload lands as a side effect of the insert. If those logs feed
 * an LLM-backed triage or summarisation pipeline, the text reaches the
 * model's context without any user interaction at all.
 *
 * Capacity is fixed by the protocol: usb_string_descriptor.bLength is
 * one byte and strings are UTF-16, giving 126 characters per string.
 * Long payloads are split across manufacturer, product and serial for
 * a ceiling of PIFK_USBDESC_TOTAL_CHARS.
 *
 * Only ASCII is transmitted. Bytes >= 0x80 would need real UTF-8
 * decoding to become correct UTF-16 code units, so they are replaced
 * with '?' and reported via pifk_usb_descriptor_lossy_chars(). */

/* Characters per string descriptor.
 *
 * usb_string_descriptor.bLength is a uint8_t covering a 2-byte header
 * plus 2 bytes per UTF-16 character, so 126 characters give bLength
 * 254. 127 would need 256 and overflow the field. */
#define PIFK_USBDESC_MAX_CHARS 126

/* Manufacturer + product + serial. */
#define PIFK_USBDESC_STRINGS     3
#define PIFK_USBDESC_TOTAL_CHARS (PIFK_USBDESC_MAX_CHARS * PIFK_USBDESC_STRINGS)

/* Start advertising `payload` in the USB identifying strings.
 *
 * Switches USB configuration, which forces the host to re-enumerate and
 * therefore to read and log the new strings. The previous configuration
 * is restored by pifk_usb_descriptor_stop().
 *
 * Returns false if the payload is empty or USB mode switching is
 * locked by another part of the system. */
bool pifk_execute_usb_descriptor(PifkApp* app, const PifkPayload* payload);

/* Restore the USB configuration that was active before the last
 * successful pifk_execute_usb_descriptor(). Safe to call when
 * nothing is active. */
void pifk_usb_descriptor_stop(PifkApp* app);

/* Number of characters in `text` that cannot be represented, counting
 * everything past PIFK_USBDESC_TOTAL_CHARS as lost too. Lets the UI
 * warn before transmitting a payload that will arrive incomplete. */
size_t pifk_usb_descriptor_lossy_chars(const char* text);
