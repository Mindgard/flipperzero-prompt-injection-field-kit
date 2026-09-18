/*
 * Host-side tests for the NFC tag builder.
 *
 * Build & run:
 *   cc -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -o tests/test_nfc_ndef tests/test_nfc_ndef.c
 *   ./tests/test_nfc_ndef
 *
 * Exits non-zero on the first failure so it can gate CI.
 *
 * The encoder cannot be linked directly the way test_qr.c links
 * qrcode.c: nfc_listener_exec.c pulls in the NFC stack, MfUltralightData
 * and furi.  The layout logic is reproduced here instead, which means
 * these tests guard the format rules rather than the shipped bytes —
 * keep them in step with pifk_nfc_write_ndef_record() and
 * pifk_nfc_init_tag().
 *
 * Two bugs motivated this file, both invisible until a real reader saw
 * the tag:
 *
 *   1. The UID was written only to the ISO14443-3A layer, leaving pages
 *      0-2 zeroed.  A reader recomputes the BCC check bytes from page 0
 *      and drops the tag on a mismatch, so an iPhone ignored it
 *      completely — no error, no partial read.
 *
 *   2. The record and TLV length fields were cast into single bytes with
 *      no range check, so text over 248 characters wrapped (256 became
 *      0) and produced a tag that read as an empty record.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Mirrors of the constants under test ─────────────────────── */

#define PAGE_SIZE       4
#define PAGES_TOTAL     135 /* NTAG215 */
#define USER_PAGE_FIRST 4
#define USER_PAGES      126
#define USER_BYTES      (USER_PAGES * PAGE_SIZE)

/* The capability container declares the NDEF area as size/8; a genuine
 * NTAG215 reports 0x3E, so 496 bytes rather than the full 504. */
#define CC_SIZE_BYTE    0x3E
#define NDEF_AREA_BYTES (CC_SIZE_BYTE * 8)
#define NDEF_OVERHEAD   15
#define MAX_TEXT        (NDEF_AREA_BYTES - NDEF_OVERHEAD)

static const uint8_t kUid[7] = {0x04, 0x4D, 0x47, 0x4D, 0x47, 0x4B, 0x54};

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if(!(cond)) {                                     \
            failures++;                                   \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while(0)

/* ── Tag under test ──────────────────────────────────────────── */

static uint8_t page[PAGES_TOTAL][PAGE_SIZE];

/* Byte n of the NDEF area, which starts at page 4. Must be a function:
 * as a macro, byte(i++) would increment i twice, once per subscript. */
static uint8_t byte(size_t i) {
    return page[USER_PAGE_FIRST + i / PAGE_SIZE][i % PAGE_SIZE];
}

/* Mirrors mf_ultralight_set_uid(): the UID is repeated across pages 0-1
 * and the two BCC check bytes land in page 0 byte 3 and page 2 byte 0. */
static void set_uid(const uint8_t* uid) {
    memcpy(page[0], uid, 3);
    memcpy(page[1], &uid[3], 4);
    page[0][3] = 0x88 ^ uid[0] ^ uid[1] ^ uid[2];
    page[2][0] = uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
}

static void init_tag(void) {
    memset(page, 0, sizeof(page));
    set_uid(kUid);
    page[2][1] = 0x48; /* internal byte */
    page[3][0] = 0xE1; /* NDEF magic */
    page[3][1] = 0x10; /* version 1.0 */
    page[3][2] = CC_SIZE_BYTE;
    page[3][3] = 0x00; /* read/write */
}

/* Mirrors pifk_nfc_write_ndef_record(). */
static int write_record(char type, const uint8_t* body, size_t body_len) {
    const int long_record = body_len > 0xFF;
    const size_t record_header = long_record ? 7 : 4;
    const size_t record_len = record_header + body_len;
    const int long_tlv = record_len >= 0xFF;
    const size_t total = 1 + (long_tlv ? 3 : 1) + record_len + 1;
    if(total > NDEF_AREA_BYTES) return 0;

    uint8_t buf[USER_BYTES];
    memset(buf, 0, sizeof(buf));

    size_t i = 0;
    buf[i++] = 0x03;
    if(long_tlv) {
        buf[i++] = 0xFF;
        buf[i++] = (uint8_t)(record_len >> 8);
        buf[i++] = (uint8_t)(record_len & 0xFF);
    } else {
        buf[i++] = (uint8_t)record_len;
    }
    buf[i++] = long_record ? 0xC1 : 0xD1;
    buf[i++] = 0x01;
    if(long_record) {
        buf[i++] = (uint8_t)(body_len >> 24);
        buf[i++] = (uint8_t)(body_len >> 16);
        buf[i++] = (uint8_t)(body_len >> 8);
        buf[i++] = (uint8_t)(body_len & 0xFF);
    } else {
        buf[i++] = (uint8_t)body_len;
    }
    buf[i++] = (uint8_t)type;
    memcpy(&buf[i], body, body_len);
    i += body_len;
    buf[i++] = 0xFE;
    if(i != total) {
        printf("  FAIL length bookkeeping: wrote %zu, computed %zu\n", i, total);
        failures++;
        return 0;
    }

    for(size_t p = 0; p < USER_PAGES; p++)
        memcpy(page[USER_PAGE_FIRST + p], &buf[p * PAGE_SIZE], PAGE_SIZE);
    return 1;
}

static int build_text(const char* text) {
    size_t n = strlen(text);
    if(n > MAX_TEXT) return 0;
    uint8_t body[MAX_TEXT + 3];
    body[0] = 0x02; /* UTF-8, 2-byte language code */
    body[1] = 'e';
    body[2] = 'n';
    memcpy(&body[3], text, n);
    return write_record('T', body, n + 3);
}

static int build_url(const char* url) {
    uint8_t prefix;
    const char* b;
    if(!strncmp(url, "https://", 8)) {
        prefix = 0x04;
        b = url + 8;
    } else if(!strncmp(url, "http://", 7)) {
        prefix = 0x03;
        b = url + 7;
    } else {
        prefix = 0x00;
        b = url;
    }
    size_t n = strlen(b);
    if(n > MAX_TEXT) return 0;
    uint8_t body[MAX_TEXT + 1];
    body[0] = prefix;
    memcpy(&body[1], b, n);
    return write_record('U', body, n + 1);
}

/* ── A reader's view ─────────────────────────────────────────── */

/* Parse the NDEF area back and recover the text, the way a phone would.
 * Returns 0 on any malformed field. */
static int parse_text(char* out, size_t out_len, size_t* got) {
    if(byte(0) != 0x03) return 0;

    size_t i;
    size_t record_len;
    if(byte(1) == 0xFF) {
        record_len = ((size_t)byte(2) << 8) | byte(3);
        i = 4;
    } else {
        record_len = byte(1);
        i = 2;
    }

    uint8_t flags = byte(i++);
    int short_record = flags & 0x10;
    if((flags & 0x07) != 0x01) return 0; /* TNF must be well-known */
    if(byte(i++) != 0x01) return 0; /* type length */

    size_t payload_len;
    if(short_record) {
        payload_len = byte(i++);
    } else {
        payload_len = ((size_t)byte(i) << 24) | ((size_t)byte(i + 1) << 16) |
                      ((size_t)byte(i + 2) << 8) | byte(i + 3);
        i += 4;
    }
    if(byte(i++) != 'T') return 0;

    /* The two length fields have to agree, or a reader gives up. */
    if(record_len != (short_record ? 4u : 7u) + payload_len) return 0;

    uint8_t status = byte(i++);
    size_t lang_len = status & 0x3F;
    i += lang_len;

    size_t text_len = payload_len - 1 - lang_len;
    if(text_len >= out_len) return 0;
    for(size_t k = 0; k < text_len; k++)
        out[k] = (char)byte(i + k);
    out[text_len] = '\0';

    if(byte(i + text_len) != 0xFE) return 0; /* terminator */
    *got = text_len;
    return 1;
}

/* ── Tests ───────────────────────────────────────────────────── */

static void test_bcc(void) {
    printf("BCC and page layout\n");
    init_tag();

    uint8_t bcc0 = 0x88 ^ kUid[0] ^ kUid[1] ^ kUid[2];
    uint8_t bcc1 = kUid[3] ^ kUid[4] ^ kUid[5] ^ kUid[6];

    CHECK(page[0][3] == bcc0, "BCC0 is %02X, expected %02X", page[0][3], bcc0);
    CHECK(page[2][0] == bcc1, "BCC1 is %02X, expected %02X", page[2][0], bcc1);

    /* A zero BCC is indistinguishable from a page nobody wrote, which is
     * how the original bug stayed hidden. Keep the UID clear of that. */
    CHECK(bcc0 != 0, "BCC0 is zero; pick a UID that cannot mask an unwritten page");
    CHECK(bcc1 != 0, "BCC1 is zero; pick a UID that cannot mask an unwritten page");

    /* A reader cross-checks the UID it learned during anticollision
     * against pages 0-1. */
    uint8_t recovered[7];
    memcpy(recovered, page[0], 3);
    memcpy(&recovered[3], page[1], 4);
    CHECK(memcmp(recovered, kUid, 7) == 0, "UID in pages 0-1 does not match");

    CHECK(kUid[0] == 0x04, "UID must start with NXP's manufacturer byte");
    CHECK(page[2][1] == 0x48, "internal byte is %02X, expected 48", page[2][1]);
    CHECK(page[3][0] == 0xE1, "CC magic is %02X, expected E1", page[3][0]);
    CHECK(page[3][1] == 0x10, "CC version is %02X, expected 10", page[3][1]);
    CHECK(page[3][2] == CC_SIZE_BYTE, "CC size is %02X, expected %02X", page[3][2], CC_SIZE_BYTE);

    /* Pinned to what a genuine NTAG215 reports, read off a real tag
     * (page 3 = E1 10 3E 00).  Deriving the limit from this byte alone
     * would let a wrong value stay self-consistent, so assert the
     * literal: 0x3F would claim all 504 bytes of user memory and
     * overstate the area by one page. */
    CHECK(CC_SIZE_BYTE == 0x3E, "CC size byte drifted from the hardware value 0x3E");
    CHECK(NDEF_AREA_BYTES == 496, "NDEF area is %d, expected 496", NDEF_AREA_BYTES);
    CHECK(MAX_TEXT == 481, "max text is %d, expected 481", MAX_TEXT);
}

static void test_capacity(void) {
    printf("capacity bounds\n");

    /* The declared limit and what the encoder accepts must agree, or the
     * kit advertises characters it then refuses. */
    char big[MAX_TEXT + 8];
    memset(big, 'A', sizeof(big));

    big[MAX_TEXT] = '\0';
    init_tag();
    CHECK(build_text(big), "text of exactly MAX_TEXT (%d) was refused", MAX_TEXT);

    /* Re-fill: the terminator above is inside the buffer, so it has to be
     * overwritten before the string can grow. */
    memset(big, 'A', sizeof(big));
    big[MAX_TEXT + 1] = '\0';
    CHECK(!build_text(big), "text of MAX_TEXT+1 was accepted");

    /* The message must fit the area the CC declares, not user memory. */
    CHECK(
        MAX_TEXT + NDEF_OVERHEAD == NDEF_AREA_BYTES,
        "overhead accounting drifted: %d + %d != %d",
        MAX_TEXT,
        NDEF_OVERHEAD,
        NDEF_AREA_BYTES);
    CHECK(
        NDEF_AREA_BYTES <= USER_BYTES,
        "CC declares %d bytes but user memory is only %d",
        NDEF_AREA_BYTES,
        USER_BYTES);
}

static void test_roundtrip(void) {
    printf("round-trip across length boundaries\n");

    /* 248/249 straddle the short/long TLV switch, 252/253 the short/long
     * record switch. Both were silently truncating before. */
    const int sizes[] = {
        0, 1, 10, 100, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 300, 400, MAX_TEXT};

    char in[MAX_TEXT + 8];
    char out[MAX_TEXT + 8];

    for(size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        int n = sizes[k];
        memset(in, 'A', (size_t)n);
        in[n] = '\0';

        init_tag();
        if(!build_text(in)) {
            CHECK(0, "n=%d: build refused", n);
            continue;
        }

        size_t got = 0;
        if(!parse_text(out, sizeof(out), &got)) {
            CHECK(0, "n=%d: parse failed", n);
            continue;
        }
        CHECK(got == (size_t)n, "n=%d: recovered %zu bytes", n, got);
        CHECK(strcmp(in, out) == 0, "n=%d: text differs after round-trip", n);
    }
}

static void test_url(void) {
    printf("URI records\n");

    struct {
        const char* url;
        uint8_t prefix;
        const char* body;
    } cases[] = {
        {"https://app.example.com/r/abc", 0x04, "app.example.com/r/abc"},
        {"http://x.io/a", 0x03, "x.io/a"},
        {"ftp://x.io/a", 0x00, "ftp://x.io/a"},
    };

    for(size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        init_tag();
        CHECK(build_url(cases[k].url), "%s: refused", cases[k].url);

        /* Short record for these lengths: type at offset 5, prefix at 6. */
        CHECK(byte(5) == 'U', "%s: type is %02X, expected U", cases[k].url, byte(5));
        CHECK(
            byte(6) == cases[k].prefix,
            "%s: prefix is %02X, expected %02X",
            cases[k].url,
            byte(6),
            cases[k].prefix);

        size_t body_len = strlen(cases[k].body);
        CHECK(
            byte(4) == body_len + 1,
            "%s: payload length is %u, expected %zu",
            cases[k].url,
            byte(4),
            body_len + 1);

        for(size_t j = 0; j < body_len; j++) {
            if(byte(7 + j) != (uint8_t)cases[k].body[j]) {
                CHECK(0, "%s: body differs at %zu", cases[k].url, j);
                break;
            }
        }
    }
}

static void test_ndef_area_clear(void) {
    printf("unused area is cleared\n");

    /* A short payload must leave the rest of the area zeroed. Garbage
     * after the terminator is how an uninitialised malloc shows up, and
     * mf_ultralight_alloc() does not zero the struct. */
    init_tag();
    CHECK(build_text("hi"), "short text refused");

    size_t message_end = 2 + 4 + 5 + 1; /* TLV + header + body + terminator */
    for(size_t i = message_end; i < USER_BYTES; i++) {
        if(byte(i) != 0) {
            CHECK(0, "byte %zu after the message is %02X, expected 00", i, byte(i));
            break;
        }
    }

    /* Pages past user memory belong to the config area and must not have
     * been scribbled on. */
    for(size_t p = USER_PAGE_FIRST + USER_PAGES; p < PAGES_TOTAL; p++) {
        if(page[p][0] || page[p][1] || page[p][2] || page[p][3]) {
            CHECK(0, "page %zu outside user memory is not clear", p);
            break;
        }
    }
}

/* ── Percent-encoding for the URI record ─────────────────────── */

/* Mirrors pifk_nfc_url_encode(). Returns false if the encoded result
 * would not fit — reporting success separately from the length, because
 * an empty payload legitimately encodes to zero bytes and conflating the
 * two made a zero-length payload look like an overflow. */
static bool url_encode(char* out, size_t out_size, const char* text) {
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

#define URL_PREFIX "https://example.com/?q="

static bool build_payload_url(char* out, size_t out_size, const char* text) {
    const size_t prefix_len = strlen(URL_PREFIX);
    if(prefix_len >= out_size) return false;
    memcpy(out, URL_PREFIX, prefix_len);
    return url_encode(out + prefix_len, out_size - prefix_len, text);
}

static void test_url_encoding(void) {
    printf("percent-encoding\n");

    char url[MAX_TEXT + 1];

    /* An empty payload is not an error. Returning the encoded length as
     * the success flag made this case indistinguishable from overflow. */
    CHECK(build_payload_url(url, sizeof(url), ""), "empty payload was rejected");
    CHECK(strcmp(url, URL_PREFIX) == 0, "empty payload produced \"%s\"", url);

    /* The unreserved set stays literal; everything else escapes. */
    CHECK(build_payload_url(url, sizeof(url), "abc-XYZ_0.9~"), "unreserved payload rejected");
    CHECK(strcmp(url, URL_PREFIX "abc-XYZ_0.9~") == 0, "unreserved set was escaped: %s", url);

    /* Payloads here are adversarial by design, so the characters that
     * would break a URL are exactly the ones that turn up. */
    CHECK(
        build_payload_url(url, sizeof(url), "a\"b<c>d\ne&f=g#h?i/j\\k ="),
        "adversarial payload rejected");
    for(const char* p = url + strlen(URL_PREFIX); *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~' || c == '%';
        if(!safe) {
            CHECK(0, "unescaped byte %02X survived encoding", c);
            break;
        }
    }

    /* Spot-check a couple of escapes rather than the whole string, so the
     * test does not just restate the implementation. */
    CHECK(build_payload_url(url, sizeof(url), "<>"), "angle brackets rejected");
    CHECK(strcmp(url, URL_PREFIX "%3C%3E") == 0, "expected %%3C%%3E, got %s", url);

    /* Overflow must be refused rather than truncated: a half-encoded
     * payload would deliver silently mangled text. */
    char big[MAX_TEXT * 2];
    memset(big, '"', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(!build_payload_url(url, sizeof(url), big), "oversize payload was accepted");

    /* The two ceilings the header documents. Worst case is every
     * character escaping, which costs three bytes each. */
    int best = -1, worst = -1;
    for(int n = 0; n <= MAX_TEXT; n++) {
        memset(big, 'A', (size_t)n);
        big[n] = '\0';
        if(!build_payload_url(url, sizeof(url), big)) break;
        best = n;
    }
    for(int n = 0; n <= MAX_TEXT; n++) {
        memset(big, ' ', (size_t)n);
        big[n] = '\0';
        if(!build_payload_url(url, sizeof(url), big)) break;
        worst = n;
    }
    printf("  unreserved ceiling %d, all-escaped ceiling %d\n", best, worst);
    CHECK(best > worst, "unreserved payloads should reach further than escaped ones");
    CHECK(worst > 0, "no payload fits once escaped");

    /* A URL built this way must still fit the tag as a URI record. */
    memset(big, 'A', (size_t)best);
    big[best] = '\0';
    CHECK(build_payload_url(url, sizeof(url), big), "ceiling payload rejected by encoder");
    init_tag();
    CHECK(build_url(url), "ceiling URL did not fit the tag as a URI record");
}

int main(void) {
    printf("NFC NDEF tag builder tests\n\n");

    test_bcc();
    test_capacity();
    test_roundtrip();
    test_url();
    test_url_encoding();
    test_ndef_area_clear();

    printf("\n%d checks, %d failures\n", checks, failures);
    if(failures == 0) printf("max text %d bytes, NDEF area %d bytes\n", MAX_TEXT, NDEF_AREA_BYTES);
    return failures ? 1 : 0;
}
