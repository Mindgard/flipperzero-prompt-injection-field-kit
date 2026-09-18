/*
 * In-process NDEF tag emulation.
 *
 * nfc_emulate.c writes a .nfc file and hands over to the stock NFC app.
 * That is fine as far as it goes, but the operator loses their place and
 * we lose any completion signal, because we have exited by the time the
 * tag is presented. This does the emulation here instead.
 *
 * The tag is an NTAG215 with a Text record in user memory. NTAG215
 * rather than NTAG213 for the extra capacity, and rather than NTAG216
 * because 504 bytes already exceeds anything in the payload database and
 * a smaller tag reads faster.
 *
 * The NDEF encoder here is the single source of truth for both paths:
 * nfc_emulate.c builds its saved tags with the same builder, so a tag
 * behaves identically whether it was emulated in-process or written to
 * a .nfc file and opened from the stock app.
 */

#include "nfc_listener_exec.h"
#include <nfc/nfc.h>
#include <nfc/nfc_listener.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <string.h>

static Nfc* pifk_nfc = NULL;
static NfcListener* pifk_nfc_listener = NULL;
static MfUltralightData* pifk_nfc_data = NULL;

/* NTAG215 identity. The UID is ours rather than a clone of a real tag:
 * a reader will see a valid NTAG215, but nothing is impersonating a
 * specific card someone else owns.
 *
 * 0x04 is NXP's manufacturer byte, which a reader expects on an NTAG.
 * The rest spells "MG"/"MGKT" so the kit is recognisable in a capture.
 * Both BCC check bytes come out non-zero with these values (0x86, 0x15),
 * which is deliberate: a UID whose BCC is zero looks identical to a page
 * we forgot to write, and that is the bug this UID used to hide. */
static const uint8_t pifk_nfc_uid[7] = {0x04, 0x4D, 0x47, 0x4D, 0x47, 0x4B, 0x54};

/* ── NDEF construction ───────────────────────────────────────── */

/* Lay an NDEF message out over the tag's user pages.
 *
 * `record_type` is the single-byte well-known type ('T' for Text, 'U'
 * for URI); `body` is that record's payload, already including whatever
 * prefix the type requires — the status and language bytes for Text, the
 * URI abbreviation code for URI.
 *
 * Layout, starting at page 4:
 *   03 <len>              NDEF Message TLV
 *   D1 01 <plen> <type>   record header, type length 1
 *   <body bytes>
 *   FE                    terminator TLV
 *
 * Two length fields have to grow once the payload gets big, and both
 * have their own escape. A short record (SR=1) carries a one-byte
 * payload length; past 255 bytes the record must switch to SR=0 and a
 * four-byte big-endian length. Independently, a TLV length of 0xFF means
 * "two big-endian bytes follow" rather than a literal 255. NTAG215 holds
 * 504 bytes of user memory, so the long forms are reachable and a
 * short-only encoder would cap payloads at 248 characters.
 *
 * Returns false if the message does not fit in user memory. */
static bool pifk_nfc_write_ndef_record(
    MfUltralightData* data,
    char record_type,
    const uint8_t* body,
    size_t body_len) {
    const bool long_record = body_len > 0xFF;

    /* Record = flags + type length + payload length + type + body.
     * The payload length is 1 byte when short, 4 when long. */
    const size_t record_header = long_record ? 7 : 4;
    const size_t record_len = record_header + body_len;
    const bool long_tlv = record_len >= 0xFF;

    /* Message = TLV header + record + terminator TLV. Bound against the
     * area the capability container declares, not the raw user memory:
     * bytes past it are ignored by a conforming reader. */
    const size_t total = 1 + (long_tlv ? 3 : 1) + record_len + 1;
    if(total > PIFK_NFC_NDEF_AREA_BYTES) return false;

    uint8_t buf[PIFK_NFC_USER_BYTES];
    memset(buf, 0, sizeof(buf));

    size_t i = 0;
    buf[i++] = 0x03; /* NDEF Message TLV */
    if(long_tlv) {
        buf[i++] = 0xFF; /* escape: 16-bit length follows */
        buf[i++] = (uint8_t)(record_len >> 8);
        buf[i++] = (uint8_t)(record_len & 0xFF);
    } else {
        buf[i++] = (uint8_t)record_len;
    }

    /* MB=1 ME=1 CF=0 IL=0 TNF=1 (well-known); SR set only when short. */
    buf[i++] = long_record ? 0xC1 : 0xD1;
    buf[i++] = 0x01; /* type length */
    if(long_record) {
        buf[i++] = (uint8_t)(body_len >> 24);
        buf[i++] = (uint8_t)(body_len >> 16);
        buf[i++] = (uint8_t)(body_len >> 8);
        buf[i++] = (uint8_t)(body_len & 0xFF);
    } else {
        buf[i++] = (uint8_t)body_len;
    }
    buf[i++] = (uint8_t)record_type;

    memcpy(&buf[i], body, body_len);
    i += body_len;
    buf[i++] = 0xFE; /* terminator */
    furi_assert(i == total);

    /* Copy into user pages. */
    for(size_t page = 0; page < PIFK_NFC_USER_PAGES; page++) {
        memcpy(
            data->page[PIFK_NFC_USER_PAGE_FIRST + page].data,
            &buf[page * MF_ULTRALIGHT_PAGE_SIZE],
            MF_ULTRALIGHT_PAGE_SIZE);
    }
    return true;
}

/* Write `text` into `data` as a single NDEF Text record, English. */
static bool pifk_nfc_write_ndef(MfUltralightData* data, const char* text) {
    size_t text_len = strlen(text);
    if(text_len > PIFK_NFC_MAX_TEXT) return false;

    uint8_t body[PIFK_NFC_MAX_TEXT + 3];
    body[0] = 0x02; /* UTF-8, language code length 2 */
    body[1] = 'e';
    body[2] = 'n';
    memcpy(&body[3], text, text_len);

    return pifk_nfc_write_ndef_record(data, 'T', body, text_len + 3);
}

/* Fill in the parts of the tag a reader checks before it will look at
 * user memory at all. */
static void pifk_nfc_init_tag(MfUltralightData* data) {
    /* mf_ultralight_alloc() is a bare malloc(): only iso14443_3a_data is
     * initialised, so every field below starts as heap garbage. Clear the
     * lot before filling anything in, or unset pages, counters and the
     * signature carry whatever was on the heap. */
    data->type = MfUltralightTypeNTAG215;
    data->pages_total = mf_ultralight_get_pages_total(MfUltralightTypeNTAG215);
    data->pages_read = data->pages_total;
    data->auth_attempts = 0;
    memset(&data->signature, 0, sizeof(data->signature));
    memset(data->counter, 0, sizeof(data->counter));
    memset(data->tearing_flag, 0, sizeof(data->tearing_flag));
    memset(data->page, 0, sizeof(data->page));

    /* GET_VERSION response. A reader that asks and gets nonsense may
     * refuse to continue, so this has to be a real NTAG215 answer.
     * storage_size 0x11 is what mf_ultralight_get_type_by_version() maps
     * back to NTAG215. */
    data->version.header = 0x00;
    data->version.vendor_id = 0x04; /* NXP */
    data->version.prod_type = 0x04; /* NTAG */
    data->version.prod_subtype = 0x02;
    data->version.prod_ver_major = 0x01;
    data->version.prod_ver_minor = 0x00;
    data->version.storage_size = 0x11;
    data->version.protocol_type = 0x03;

    /* ATQA/SAK must match what mf_ultralight_detect_protocol() looks for,
     * or a reader will not treat this as an Ultralight family tag. */
    data->iso14443_3a_data->atqa[0] = 0x44;
    data->iso14443_3a_data->atqa[1] = 0x00;
    data->iso14443_3a_data->sak = 0x00;

    /* mf_ultralight_set_uid(), not iso14443_3a_set_uid(): a real NTAG
     * repeats the UID in pages 0-1 and carries the two BCC check bytes
     * in page 0 byte 3 and page 2 byte 0. The ISO14443-3A setter only
     * touches the anticollision layer, which leaves those pages zeroed —
     * a phone reads page 0, computes the BCC, sees the mismatch and drops
     * the tag before it ever reaches the NDEF data. */
    mf_ultralight_set_uid(data, pifk_nfc_uid, sizeof(pifk_nfc_uid));

    /* Page 2 byte 1 is the internal byte; 0x48 is what real NTAGs carry.
     * Bytes 2-3 are the static lock bytes, left clear so the tag stays
     * writable. */
    data->page[2].data[1] = 0x48;

    /* Capability container in page 3: NDEF magic, version 1.0, size/8,
     * read/write access. A genuine NTAG215 reports 0x3E — 496 bytes, not
     * the full 504, since the CC itself and the terminator need room. */
    data->page[3].data[0] = 0xE1;
    data->page[3].data[1] = 0x10;
    data->page[3].data[2] = 0x3E;
    data->page[3].data[3] = 0x00;
}

/* ── Public API ──────────────────────────────────────────────── */

bool pifk_nfc_build_tag(MfUltralightData* data, const char* text) {
    if(!data || !text) return false;
    pifk_nfc_init_tag(data);
    return pifk_nfc_write_ndef(data, text);
}

bool pifk_nfc_build_url_tag(MfUltralightData* data, const char* url) {
    if(!data || !url) return false;

    /* The URI record's first byte abbreviates a common scheme, so the
     * scheme itself is not stored. 0x04 is "https://", 0x03 "http://",
     * 0x00 means the body carries the whole URI verbatim. */
    uint8_t prefix;
    const char* body_str;
    if(strncmp(url, "https://", 8) == 0) {
        prefix = 0x04;
        body_str = url + 8;
    } else if(strncmp(url, "http://", 7) == 0) {
        prefix = 0x03;
        body_str = url + 7;
    } else {
        prefix = 0x00;
        body_str = url;
    }

    size_t body_len = strlen(body_str);
    if(body_len > PIFK_NFC_MAX_TEXT) return false;

    uint8_t body[PIFK_NFC_MAX_TEXT + 1];
    body[0] = prefix;
    memcpy(&body[1], body_str, body_len);

    pifk_nfc_init_tag(data);
    return pifk_nfc_write_ndef_record(data, 'U', body, body_len + 1);
}

/* Percent-encode `text` into `out` as a URI query value.
 *
 * Everything outside the unreserved set is escaped, which is stricter
 * than necessary but keeps the result safe in a query string whatever
 * the payload contains — and payloads here are adversarial by design:
 * quotes, newlines, angle brackets and non-ASCII are all normal.
 *
 * Returns false if the encoded result would not fit. Success is reported
 * separately from the length because an empty payload legitimately
 * encodes to zero bytes, and conflating the two made a zero-length
 * payload look like an overflow. */
static bool pifk_nfc_url_encode(char* out, size_t out_size, const char* text) {
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;

    for(const char* p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if(unreserved) {
            if(w + 1 >= out_size) return false;
            out[w++] = (char)c;
        } else {
            if(w + 3 >= out_size) return false;
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 0x0F];
        }
    }

    if(w >= out_size) return false;
    out[w] = '\0';
    return true;
}

/* Build the URL a URI-record tap opens.
 *
 * example.com is reserved by RFC 2606 for exactly this: it is guaranteed
 * not to resolve to anyone's infrastructure, so a stray tap during
 * testing does not send an operator's payload to a live host. The
 * injection text is the query value, which is what a phone displays and
 * what anything reading the URL ingests. */
#define PIFK_NFC_URL_PREFIX "https://example.com/?q="

static bool pifk_nfc_build_payload_url(char* out, size_t out_size, const char* text) {
    const size_t prefix_len = strlen(PIFK_NFC_URL_PREFIX);
    if(prefix_len >= out_size) return false;

    memcpy(out, PIFK_NFC_URL_PREFIX, prefix_len);
    return pifk_nfc_url_encode(out + prefix_len, out_size - prefix_len, text);
}

bool pifk_execute_nfc_listener(PifkApp* app, const PifkPayload* payload, PifkNfcRecordType record) {
    if(!payload || !payload->text || !payload->text[0]) return false;

    /* Text goes in as-is. A URL carries the same text percent-encoded,
     * which costs up to three bytes per character, so the URI path is
     * bounded by the encoded length rather than the raw one — checked
     * inside pifk_nfc_build_payload_url() against the real buffer. */
    char url[PIFK_NFC_MAX_TEXT + 1];
    const char* content;

    if(record == PifkNfcRecordUri) {
        if(!pifk_nfc_build_payload_url(url, sizeof(url), payload->text)) return false;
        content = url;
    } else {
        if(strlen(payload->text) > PIFK_NFC_MAX_TEXT) return false;
        content = payload->text;
    }

    /* Re-entry: tear the old listener down first. */
    if(pifk_nfc_listener) {
        pifk_nfc_listener_stop(app);
    }

    pifk_nfc_data = mf_ultralight_alloc();
    if(!pifk_nfc_data) return false;

    const bool built = (record == PifkNfcRecordUri) ?
                           pifk_nfc_build_url_tag(pifk_nfc_data, content) :
                           pifk_nfc_build_tag(pifk_nfc_data, content);
    if(!built) {
        mf_ultralight_free(pifk_nfc_data);
        pifk_nfc_data = NULL;
        return false;
    }

    pifk_nfc = nfc_alloc();
    if(!pifk_nfc) {
        mf_ultralight_free(pifk_nfc_data);
        pifk_nfc_data = NULL;
        return false;
    }

    pifk_nfc_listener = nfc_listener_alloc(pifk_nfc, NfcProtocolMfUltralight, pifk_nfc_data);
    if(!pifk_nfc_listener) {
        nfc_free(pifk_nfc);
        pifk_nfc = NULL;
        mf_ultralight_free(pifk_nfc_data);
        pifk_nfc_data = NULL;
        return false;
    }

    /* No event callback: nothing here needs to react to reader traffic,
     * and the listener serves the tag data on its own. */
    nfc_listener_start(pifk_nfc_listener, NULL, app);
    return true;
}

void pifk_nfc_listener_stop(PifkApp* app) {
    UNUSED(app);
    if(!pifk_nfc_listener) return;

    /* Order matters: stop the worker before freeing anything it holds a
     * pointer to, or the NFC thread reads freed memory. */
    nfc_listener_stop(pifk_nfc_listener);
    nfc_listener_free(pifk_nfc_listener);
    pifk_nfc_listener = NULL;

    if(pifk_nfc) {
        nfc_free(pifk_nfc);
        pifk_nfc = NULL;
    }
    if(pifk_nfc_data) {
        mf_ultralight_free(pifk_nfc_data);
        pifk_nfc_data = NULL;
    }
}
