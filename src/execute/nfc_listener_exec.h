#pragma once

#include "../pifk_app.h"
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>

/* Emulate an NDEF tag carrying the payload, in-process.
 *
 * The existing NFC path (nfc_emulate.c) writes a .nfc file and hands
 * over to the stock NFC app via loader_enqueue_launch(). That works but
 * costs the operator their place in this app, and we get no completion
 * signal because we have exited.
 *
 * This emulates the tag ourselves using the exported nfc_listener API,
 * so the payload is presented while the kit stays on screen.
 *
 * Why NFC matters among the channels here: NDEF is the one wireless
 * format a phone acts on with no app installed and no pairing. Tap an
 * unlocked handset against an NDEF tag and the OS parses it and offers
 * the content. Of everything in the kit this is the only channel that
 * reaches a general-purpose consumer device with zero setup on the
 * target — provided the record type is one the OS surfaces, which is the
 * distinction PifkNfcRecordType exists to make.
 *
 * Emulated as NTAG215: 504 bytes of user memory, widely recognised, and
 * large enough for the kit's longest payloads. */

/* NTAG215 user memory: pages 4..129 inclusive, 4 bytes each. */
#define PIFK_NFC_USER_PAGE_FIRST 4
#define PIFK_NFC_USER_PAGES      126
#define PIFK_NFC_USER_BYTES      (PIFK_NFC_USER_PAGES * 4)

/* The NDEF area a reader will actually honour, which is smaller than the
 * 504 bytes of user memory. The capability container in page 3 states the
 * area as size/8, and a genuine NTAG215 reports 0x3E — 496 bytes. Writing
 * past that puts bytes on the tag that a conforming reader ignores, so
 * everything below sizes against this figure rather than the raw total. */
#define PIFK_NFC_NDEF_AREA_BYTES 496

/* NDEF overhead inside user memory, worst case — which is what a payload
 * near the limit actually hits:
 *
 *   1   NDEF Message TLV type (0x03)
 *   3   TLV length, long form (0xFF + 16-bit length)
 *   7   record header: flags, type length, 32-bit payload length, type
 *   3   Text record prefix: status byte + "en"
 *   1   terminator TLV (0xFE)
 *  ---
 *  15
 *
 * Small payloads use the short forms and spend only 11, but sizing
 * against that figure would advertise four characters the encoder then
 * refuses. */
#define PIFK_NFC_NDEF_OVERHEAD 15

/* Largest payload that fits an NTAG215 text record: 481 characters. */
#define PIFK_NFC_MAX_TEXT (PIFK_NFC_NDEF_AREA_BYTES - PIFK_NFC_NDEF_OVERHEAD)

/* Which NDEF record type to present.
 *
 * This is an operational choice, not a cosmetic one. A tapped iPhone
 * reads either kind — verified on the wire, the listener logs a full
 * CMD_READ sweep of the NDEF area for both — but iOS only raises a
 * banner for a URI record. A Text record is read and silently dropped
 * unless an app with an open NFCNDEFReaderSession is in the foreground.
 *
 * So Text reaches a model only where something is already listening: a
 * kiosk, an Android handset with a tag-reading app, an assistant with a
 * tag-reading integration. Uri reaches a stock, locked-down iPhone with
 * nothing installed, which is the case worth having. */
typedef enum {
    PifkNfcRecordText,
    PifkNfcRecordUri,
} PifkNfcRecordType;

/* Begin emulating a tag carrying `payload` as `record`.
 *
 * For PifkNfcRecordUri the payload is percent-encoded into a query
 * string, so what the target opens is a URL whose parameter holds the
 * injection text. Encoding costs up to three bytes a character, so the
 * usable length depends on the payload: about 458 characters of
 * unreserved ASCII, but as few as 152 if every character escapes.
 * Punctuation-heavy injection text lands between the two, and an
 * over-long payload is refused rather than truncated.
 *
 * Runs until pifk_nfc_listener_stop(). Returns false if the payload
 * is empty, does not fit the record type, or the NFC hardware could not
 * be claimed. */
bool pifk_execute_nfc_listener(PifkApp* app, const PifkPayload* payload, PifkNfcRecordType record);

/* Build an NTAG215 image whose user memory holds `text` as an NDEF text
 * record, including the capability container and GET_VERSION response a
 * reader checks before it will look at user memory.
 *
 * Shared with the .nfc file writer so both paths produce byte-identical
 * tags.
 *
 * `data` must come from mf_ultralight_alloc(). Returns false if the text
 * exceeds PIFK_NFC_MAX_TEXT. */
bool pifk_nfc_build_tag(MfUltralightData* data, const char* text);

/* As pifk_nfc_build_tag, but stores `url` as an NDEF URI record so a
 * phone offers to open it. "https://" and "http://" are folded into the
 * URI abbreviation byte; anything else is stored whole. */
bool pifk_nfc_build_url_tag(MfUltralightData* data, const char* url);

/* Stop emulating and release the NFC hardware. Safe when idle. */
void pifk_nfc_listener_stop(PifkApp* app);
