#pragma once

#include "../pifk_app.h"

/* Execute a single payload via USB HID (BadUSB).
 * Types the payload text character-by-character and presses Enter.
 * Returns true on success. */
bool pifk_execute_badusb(PifkApp* app, const PifkPayload* payload);
/* Abort any in-progress BadUSB execution. */
void pifk_badusb_abort(PifkApp* app);

/* HID keycode lookup for printable ASCII. */
uint16_t hid_ascii_to_key(char c);

/* Present as a USB HID keyboard and report whether the attached host
 * enumerated us, then restore the previous USB configuration.
 *
 * This types nothing. It exists to answer one question: will a given
 * USB host accept a generic HID keyboard? The case that motivated it is
 * a label printer's USB-A host port, where the printer is documented as
 * supporting "HID class" peripherals such as barcode scanners. If it
 * refuses an unknown keyboard, every scanner-emulation idea built on
 * top of that port is dead and worth knowing early.
 *
 * `waited_ms` receives how long enumeration took, or the full timeout if
 * it never happened. Either pointer may be NULL.
 *
 * Returns true if furi_hal_hid_is_connected() went true within the
 * timeout. */
bool pifk_badusb_probe_host(PifkApp* app, uint32_t timeout_ms, uint32_t* waited_ms);

/* Count characters in `text` that hid_ascii_to_key() cannot map.
 *
 * Those characters are skipped silently during typing, so a payload
 * using homoglyphs, zero-width spaces or any non-ASCII text arrives
 * incomplete over BadUSB while looking correct on screen.  Callers use
 * this to warn before executing rather than let it fail invisibly. */
size_t pifk_badusb_count_unmappable(const char* text);
