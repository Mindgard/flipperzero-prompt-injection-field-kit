/*
 * Host-side tests for channel eligibility — what the payload lists are
 * allowed to show.
 *
 * Build & run:
 *   cc -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -o /tmp/test_channel tests/test_channel.c
 *   /tmp/test_channel
 *
 * Exits non-zero on failure so it can gate CI.
 *
 * Why this is worth testing: under transport-first navigation the
 * filter decides what an operator can see at all.  A limit that is too
 * generous produces the truncated or mangled delivery the kit is
 * careful to refuse everywhere else; one that is too strict silently
 * hides working payloads, and there is no error message to notice
 * because filtering has no failure path.
 *
 * The capacity numbers come from src/channel/channel_limits.h, which is
 * asserted against the real definitions in channel.c — so this file
 * tests the same values the firmware enforces rather than a copy.
 *
 * The two lossy-character predicates are reimplemented here, because
 * the originals need the Flipper HID HAL and the USB HAL.  Keep them in
 * step with pifk_badusb_count_unmappable() in
 * src/execute/badusb_exec.c and pifk_usb_descriptor_lossy_chars()
 * in src/execute/usb_descriptor_exec.c.  That they differ is the point
 * of the test: the table stores a function pointer per channel rather
 * than one ascii_only flag precisely because these two disagree.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../src/channel/channel_limits.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...)                        \
    do {                                     \
        checks++;                            \
        if(!(c)) {                           \
            fails++;                         \
            printf("  FAIL %d: ", __LINE__); \
            printf(__VA_ARGS__);             \
            puts("");                        \
        }                                    \
    } while(0)

/* ── Stubs of the two lossy-character predicates ─────────────── */

/* BadUSB: hid_ascii_to_key() has no keycode for bytes >= 0x80, nor for
 * most control characters.  Tab, newline and carriage return do map.
 * Printable ASCII 0x20-0x7E all map. */
static size_t badusb_unmappable(const char* text) {
    size_t n = 0;
    for(size_t i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        bool mappable = (c >= 0x20 && c <= 0x7E) || c == '\t' || c == '\n' || c == '\r';
        if(!mappable) n++;
    }
    return n;
}

/* USB descriptor: only bytes >= 0x80 are unrepresentable, plus anything
 * past the three-string ceiling. */
static size_t usbdesc_lossy(const char* text) {
    size_t lossy = 0, len = 0;
    for(size_t i = 0; text[i]; i++) {
        len++;
        if((unsigned char)text[i] >= 0x80) lossy++;
    }
    if(len > PIFK_LIMIT_USBDESC) lossy += len - PIFK_LIMIT_USBDESC;
    return lossy;
}

/* ── The predicate under test ────────────────────────────────── *
 *
 * Mirrors pifk_channel_accepts_text() in src/channel/channel.c. */
static bool accepts(size_t max_bytes, size_t (*lossy)(const char*), const char* text) {
    if(!text) return false;
    size_t len = strlen(text);
    if(len == 0) return false;
    if(len > max_bytes) return false;
    if(lossy && lossy(text) > 0) return false;
    return true;
}

static void fill(char* buf, size_t n, char c) {
    memset(buf, c, n);
    buf[n] = '\0';
}

int main(void) {
    static char buf[1024];

    puts("channel eligibility");

    /* ── Boundaries: at, one under, one over ── */
    fill(buf, PIFK_LIMIT_QR, 'a');
    CHECK(accepts(PIFK_LIMIT_QR, NULL, buf), "QR should accept exactly %d", PIFK_LIMIT_QR);
    fill(buf, PIFK_LIMIT_QR - 1, 'a');
    CHECK(accepts(PIFK_LIMIT_QR, NULL, buf), "QR should accept limit-1");
    fill(buf, PIFK_LIMIT_QR + 1, 'a');
    CHECK(!accepts(PIFK_LIMIT_QR, NULL, buf), "QR must refuse limit+1");

    fill(buf, PIFK_LIMIT_NFC, 'a');
    CHECK(accepts(PIFK_LIMIT_NFC, NULL, buf), "NFC should accept exactly %d", PIFK_LIMIT_NFC);
    fill(buf, PIFK_LIMIT_NFC + 1, 'a');
    CHECK(!accepts(PIFK_LIMIT_NFC, NULL, buf), "NFC must refuse limit+1");

    fill(buf, PIFK_LIMIT_GATT, 'a');
    CHECK(accepts(PIFK_LIMIT_GATT, NULL, buf), "GATT should accept exactly %d", PIFK_LIMIT_GATT);
    fill(buf, PIFK_LIMIT_GATT + 1, 'a');
    CHECK(!accepts(PIFK_LIMIT_GATT, NULL, buf), "GATT must refuse limit+1");

    /* ── Empty payload is refused everywhere ── */
    CHECK(!accepts(PIFK_LIMIT_TEXT_BUF, NULL, ""), "empty must be refused (no lossy fn)");
    CHECK(!accepts(PIFK_LIMIT_TEXT_BUF, badusb_unmappable, ""), "empty must be refused (BadUSB)");

    /* ── The two lossy predicates genuinely differ ──
     *
     * This is the reason the table holds a function pointer per channel
     * instead of a single ascii_only flag. A newline is typable over
     * BadUSB and carryable in a descriptor; a control byte like 0x01 is
     * neither typable nor... actually fine in a descriptor. If these two
     * ever collapse to the same rule, the flag would have been correct
     * and this test should be the thing that says so. */
    const char* with_tab = "hello\tworld";
    CHECK(badusb_unmappable(with_tab) == 0, "tab should be typable");
    CHECK(usbdesc_lossy(with_tab) == 0, "tab should be carryable in a descriptor");

    const char* with_ctrl = "hello\x01world";
    CHECK(badusb_unmappable(with_ctrl) == 1, "0x01 has no HID keycode");
    CHECK(usbdesc_lossy(with_ctrl) == 0, "0x01 is representable in a UTF-16 descriptor");
    CHECK(
        !accepts(PIFK_LIMIT_TEXT_BUF, badusb_unmappable, with_ctrl),
        "BadUSB must refuse a control character");
    CHECK(
        accepts(PIFK_LIMIT_USBDESC, usbdesc_lossy, with_ctrl),
        "USB descriptor must accept what BadUSB refuses — the predicates differ");

    /* ── Non-ASCII: refused by both, for different reasons ── */
    const char* homoglyph = "\xd0\xa1ontinue"; /* Cyrillic С, 2 UTF-8 bytes */
    CHECK(badusb_unmappable(homoglyph) == 2, "both UTF-8 bytes are unmappable");
    CHECK(usbdesc_lossy(homoglyph) == 2, "both UTF-8 bytes are >= 0x80");
    CHECK(
        !accepts(PIFK_LIMIT_TEXT_BUF, badusb_unmappable, homoglyph),
        "BadUSB must refuse a homoglyph payload");
    CHECK(
        !accepts(PIFK_LIMIT_USBDESC, usbdesc_lossy, homoglyph),
        "USB descriptor must refuse a homoglyph payload");

    const char* zwsp = "ignore\xe2\x80\x8bprevious"; /* zero-width space */
    CHECK(badusb_unmappable(zwsp) == 3, "zero-width space is 3 unmappable bytes");
    CHECK(
        !accepts(PIFK_LIMIT_TEXT_BUF, badusb_unmappable, zwsp),
        "BadUSB must refuse a zero-width payload");

    /* Channels with no lossy predicate carry those bytes verbatim —
     * which is why the manual points at QR, NFC and GPIO for these. */
    CHECK(accepts(PIFK_LIMIT_NFC, NULL, homoglyph), "NFC carries arbitrary bytes");
    CHECK(accepts(PIFK_LIMIT_TEXT_BUF, NULL, zwsp), "GPIO carries arbitrary bytes");

    /* ── USB descriptor: over-length counts as lossy, so the length
     *    check and the lossy check must agree rather than double-count ── */
    fill(buf, PIFK_LIMIT_USBDESC + 10, 'a');
    CHECK(
        usbdesc_lossy(buf) == 10,
        "10 chars past the ceiling are lossy, got %zu",
        usbdesc_lossy(buf));
    CHECK(
        !accepts(PIFK_LIMIT_USBDESC, usbdesc_lossy, buf),
        "over-length must be refused by the length check");

    /* ── The QR asymmetry the whole redesign rests on ──
     *
     * Real payload sizes run 43-373 bytes. Pin the consequence: a
     * mid-sized payload fits every channel except QR. */
    fill(buf, 200, 'a');
    CHECK(!accepts(PIFK_LIMIT_QR, NULL, buf), "200B must not fit QR");
    CHECK(accepts(PIFK_LIMIT_NFC, NULL, buf), "200B must fit NFC");
    CHECK(accepts(PIFK_LIMIT_GATT, NULL, buf), "200B must fit GATT");
    CHECK(accepts(PIFK_LIMIT_USBDESC, usbdesc_lossy, buf), "200B must fit USB descriptor");
    CHECK(accepts(PIFK_LIMIT_TEXT_BUF, badusb_unmappable, buf), "200B must fit BadUSB");

    /* A 373-byte payload (the largest shipped) still fits the descriptor
     * ceiling of 378 — a boundary close enough to be worth pinning. */
    fill(buf, 373, 'a');
    CHECK(accepts(PIFK_LIMIT_USBDESC, usbdesc_lossy, buf), "373B must fit USB descriptor (378)");

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
