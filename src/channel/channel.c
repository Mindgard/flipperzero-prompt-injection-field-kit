#include "channel.h"
#include "pifk_icons.h"
#include "channel_limits.h"
#include "../pifk_app.h"
#include "../execute/badusb_exec.h"
#include "../execute/usb_descriptor_exec.h"
#include "../execute/ble_gatt_exec.h"
#include "../execute/nfc_listener_exec.h"
#include <string.h>

/* channel_limits.h is a HAL-free mirror of these limits so the host-side
 * test can include it without the Flipper headers.  These assertions are
 * what stop it becoming a second source of truth: change a limit next to
 * the code that enforces it and the build fails here until the mirror
 * agrees. */
_Static_assert(PIFK_LIMIT_TEXT_BUF == PIFK_MAX_TEXT_LEN, "text buffer limit drifted");
_Static_assert(PIFK_LIMIT_QR == PIFK_QR_MAX_BYTES, "QR limit drifted");
_Static_assert(PIFK_LIMIT_USBDESC == PIFK_USBDESC_TOTAL_CHARS, "USB desc limit drifted");
_Static_assert(PIFK_LIMIT_NFC == PIFK_NFC_MAX_TEXT, "NFC limit drifted");
_Static_assert(PIFK_LIMIT_GATT == PIFK_GATT_TOTAL_BYTES, "GATT limit drifted");

/* ── The table ───────────────────────────────────────────────── *
 *
 * Ordered by group, and within a group by how likely an operator is to
 * reach for it.  The capacity figures are all references, never
 * literals: each limit lives next to the code that enforces it, so this
 * table cannot drift from the executors.
 *
 * NFC (emulate card) is the one channel whose limit is not its own —
 * pifk_nfc_write_file() uses the same NDEF builder as emulation, so
 * it inherits PIFK_NFC_MAX_TEXT.
 */
static const PifkChannel channels[] = {
    /* ── USB: a cable into a port ── */
    {
        .id = PifkChannelBadUsb,
        .group = PifkGroupUsb,
        .label = "Type it",
        .tech = "BadUSB",
        .verb = "BADUSB",
        .help = "Types the payload as a USB\n"
                "keyboard, then presses\n"
                "Enter.\n\n"
                "The target needs a focused\n"
                "text field: a chat box, a\n"
                "search bar, a terminal. It\n"
                "types blind and cannot see\n"
                "what it hit.\n\n"
                "Non-ASCII has no HID\n"
                "keycode and is dropped, so\n"
                "homoglyph and zero-width\n"
                "payloads are not offered\n"
                "here. Use QR, NFC or UART\n"
                "for those.\n\n"
                "Stop cannot interrupt it\n"
                "mid-type.",
        .icon = &I_mg_keyboard_9x9,
        .max_bytes = PIFK_MAX_TEXT_LEN,
        .count_lossy_chars = pifk_badusb_count_unmappable,
        .persists_after_exec = false,
        .is_diagnostic = false,
    },
    {
        .id = PifkChannelUsbDesc,
        .group = PifkGroupUsb,
        .label = "Name it",
        .tech = "USB descriptor",
        .verb = "USBDESC",
        .help = "Puts the payload in the USB\n"
                "manufacturer, product and\n"
                "serial strings, then waits\n"
                "to be plugged in.\n\n"
                "The host reads and logs\n"
                "those during enumeration,\n"
                "before any driver loads.\n"
                "Windows writes them to the\n"
                "Security log and the\n"
                "registry; Linux to dmesg;\n"
                "macOS to unified logging.\n\n"
                "The reader you care about\n"
                "is not the user but the\n"
                "log pipeline. If SOC triage\n"
                "is LLM-assisted, this lands\n"
                "with no user interaction\n"
                "at all.\n\n"
                "378 chars, ASCII only.\n"
                "Back restores USB.",
        .icon = &I_mg_tag_9x9,
        .max_bytes = PIFK_USBDESC_TOTAL_CHARS,
        .count_lossy_chars = pifk_usb_descriptor_lossy_chars,
        /* The payload lands when the *host* enumerates, which has not
         * happened yet when the call returns. */
        .persists_after_exec = true,
        .is_diagnostic = false,
    },

    /* ── Wireless: proximity ── */
    {
        .id = PifkChannelNfcUrl,
        .group = PifkGroupWireless,
        .label = "Tap a phone",
        .tech = "NFC, URL record",
        .verb = "NFCEMUURL",
        .help = "Emulates an NTAG215 tag\n"
                "carrying the payload inside\n"
                "an example.com URL.\n\n"
                "This is the one to use\n"
                "against an untouched phone.\n"
                "iOS raises a banner for a\n"
                "URI record and offers to\n"
                "open it; it reads a text\n"
                "record and silently drops\n"
                "it unless an app is already\n"
                "listening.\n\n"
                "example.com is reserved by\n"
                "RFC 2606, so a stray tap\n"
                "cannot reach a live host.\n\n"
                "Percent-encoding costs up\n"
                "to 3 bytes a character, so\n"
                "a payload that fits as text\n"
                "may still be refused here.",
        .icon = &I_mg_phone_9x9,
        /* Percent-encoding costs up to three bytes per character, so a
         * payload that fits as text can still be refused here. The
         * encoder reports that; this bound is the text-record one. */
        .max_bytes = PIFK_NFC_MAX_TEXT,
        .count_lossy_chars = NULL,
        .persists_after_exec = true,
        .is_diagnostic = false,
    },
    {
        .id = PifkChannelNfcText,
        .group = PifkGroupWireless,
        .label = "Tap a reader",
        .tech = "NFC, text record",
        .verb = "NFCEMU",
        .help = "Emulates an NTAG215 tag\n"
                "carrying the payload as an\n"
                "NDEF text record.\n\n"
                "Use this when the far side\n"
                "is already reading tags: a\n"
                "kiosk, an Android reader\n"
                "app, an assistant with a\n"
                "tag integration.\n\n"
                "A stock phone reads the tag\n"
                "but discards a text record\n"
                "without an app in the\n"
                "foreground. For those, use\n"
                "the URL form instead.\n\n"
                "481 bytes. Back to stop.",
        .icon = &I_mg_reader_9x9,
        .max_bytes = PIFK_NFC_MAX_TEXT,
        .count_lossy_chars = NULL,
        .persists_after_exec = true,
        .is_diagnostic = false,
    },
    {
        .id = PifkChannelBleGatt,
        .group = PifkGroupWireless,
        .label = "Serve it",
        .tech = "BLE GATT",
        .verb = "BLEGATT",
        .help = "Publishes the payload in\n"
                "readable GATT\n"
                "characteristics. A client\n"
                "connects, reads, and gets\n"
                "it whole.\n\n"
                "No pairing is required,\n"
                "which is the point: device\n"
                "enumeration is something an\n"
                "assistant with discovery\n"
                "capability does routinely,\n"
                "and a characteristic full\n"
                "of text is an ingestion\n"
                "point.\n\n"
                "732 bytes over three\n"
                "characteristics, no\n"
                "chunking.\n\n"
                "Replaces the active BLE\n"
                "profile, so it cannot run\n"
                "with the beacon. Needs\n"
                "Bluetooth enabled.",
        .icon = &I_mg_serve_9x9,
        .max_bytes = PIFK_GATT_TOTAL_BYTES,
        .count_lossy_chars = NULL,
        .persists_after_exec = true,
        .is_diagnostic = false,
    },
    {
        .id = PifkChannelBleBeacon,
        .group = PifkGroupWireless,
        .label = "Broadcast it",
        .tech = "BLE beacon",
        .verb = "BLE",
        .help = "Broadcasts the payload in\n"
                "the advertised device name,\n"
                "29 bytes at a time, and\n"
                "hopes a scanner is\n"
                "listening.\n\n"
                "Reaches anything doing a\n"
                "passive scan, with no\n"
                "connection needed. But it\n"
                "arrives in fragments and\n"
                "there is no way to know if\n"
                "anything read it.\n\n"
                "Prefer GATT if the target\n"
                "can connect: same radio,\n"
                "whole payload.\n\n"
                "Runs 60s. Back to stop.",
        .icon = &I_mg_broadcast_9x9,
        /* 99 chunks x 21 bytes, but the payload buffer caps it first. */
        .max_bytes = PIFK_MAX_TEXT_LEN,
        .count_lossy_chars = NULL,
        .persists_after_exec = true,
        .is_diagnostic = false,
    },
    {
        .id = PifkChannelNfcFile,
        .group = PifkGroupWireless,
        .label = "Write .nfc file",
        .tech = "hands over to NFC app",
        .verb = "NFC",
        .help = "Writes a .nfc file to the SD\n"
                "card and hands over to the\n"
                "built-in NFC app, which\n"
                "means this app exits.\n\n"
                "Mostly superseded by the\n"
                "two emulate entries, which\n"
                "present the tag without\n"
                "losing your place.\n\n"
                "Still useful to keep a tag\n"
                "on disk, or to use the\n"
                "stock app's own emulation.",
        .icon = &I_mg_file_9x9,
        .max_bytes = PIFK_NFC_MAX_TEXT,
        .count_lossy_chars = NULL,
        .persists_after_exec = false,
        .is_diagnostic = false,
    },

    /* ── Screen: a camera pointed at the Flipper ── */
    {
        .id = PifkChannelQr,
        .group = PifkGroupScreen,
        .label = "Show a QR",
        .tech = "QR code",
        .verb = "QR",
        .help = "Draws the payload as a QR\n"
                "code on the Flipper screen.\n\n"
                "For targets that read the\n"
                "world through a camera: a\n"
                "phone assistant asked what\n"
                "a code says, a kiosk\n"
                "scanner, a robot with\n"
                "vision.\n\n"
                "Smallest capacity of any\n"
                "channel at 134 bytes, so\n"
                "only 18 of the 45 shipped\n"
                "payloads fit. The list\n"
                "shows those.\n\n"
                "Back to stop displaying.",
        .icon = &I_mg_qr_9x9,
        .max_bytes = PIFK_QR_MAX_BYTES,
        .count_lossy_chars = NULL,
        .persists_after_exec = false,
        .is_diagnostic = false,
    },

    /* ── Wires: header access and a shared ground ── */
    {
        .id = PifkChannelGpioCapture,
        .group = PifkGroupWires,
        /* Listed above plain send: it is the only channel in the kit
         * that can report whether the payload had an effect, so it is
         * the one an operator should reach for first. */
        .label = "Send + capture",
        .tech = "UART, reads reply",
        .verb = "GPIOCAP",
        .help = "Sends the payload on the\n"
                "UART TX pin, then listens\n"
                "on RX for a reply.\n\n"
                "The only channel here that\n"
                "reports whether the payload\n"
                "had an effect rather than\n"
                "just that it was delivered.\n"
                "A system prompt read off\n"
                "the wire is evidence; a\n"
                "delivery confirmation is\n"
                "not.\n\n"
                "Every capture is written to\n"
                "captures.jsonl and listed\n"
                "under Captures. Silence is\n"
                "logged too - a target that\n"
                "said nothing is a finding.\n\n"
                "Needs TX, RX and a shared\n"
                "ground. Run the loopback\n"
                "test first.",
        .icon = &I_mg_capture_9x9,
        .max_bytes = PIFK_MAX_TEXT_LEN,
        .count_lossy_chars = NULL,
        .persists_after_exec = false,
        .is_diagnostic = false,
    },
    {
        .id = PifkChannelGpio,
        .group = PifkGroupWires,
        .label = "Send it",
        .tech = "UART TX",
        .verb = "GPIO",
        .help = "Emits the payload as raw\n"
                "bytes on the UART TX pin.\n\n"
                "Speaks no protocol, which\n"
                "is what makes it the way\n"
                "into targets the kit cannot\n"
                "anticipate: an HMI's serial\n"
                "console, a kiosk debug\n"
                "header, a robot's UART.\n\n"
                "Write-only. If you want the\n"
                "target's reply, use Send +\n"
                "capture instead.\n\n"
                "GND is not optional - a\n"
                "floating ground produces\n"
                "garbage that looks exactly\n"
                "like a software bug. 3.3V;\n"
                "a 5V target needs a level\n"
                "shifter.\n\n"
                "With pacing on, Stop works\n"
                "mid-payload.",
        .icon = &I_mg_send_9x9,
        .max_bytes = PIFK_MAX_TEXT_LEN,
        .count_lossy_chars = NULL,
        .persists_after_exec = false,
        .is_diagnostic = false,
    },
    {
        .id = PifkChannelI2cWrite,
        .group = PifkGroupWires,
        .label = "Write to a chip",
        .tech = "I2C",
        .verb = "I2C",
        .help = "Writes the payload into a\n"
                "chip on the I2C bus: an\n"
                "EEPROM holding config text,\n"
                "a display's frame buffer, a\n"
                "sensor's registers.\n\n"
                "Reaches embedded targets\n"
                "with no serial console at\n"
                "all. The consumer is a\n"
                "pipeline that later reads\n"
                "device config or summarises\n"
                "what a display showed.\n\n"
                "Scan the bus first. Writing\n"
                "to a guessed address is\n"
                "destructive in a way UART\n"
                "is not - an unexpected\n"
                "device might be a PMIC, and\n"
                "payload text in its control\n"
                "registers is a bricked\n"
                "board.\n\n"
                "Sent in 16-byte chunks.\n"
                "Stop works between them,\n"
                "but bytes already written\n"
                "cannot be taken back.",
        .icon = &I_mg_chip_9x9,
        .max_bytes = PIFK_MAX_TEXT_LEN,
        .count_lossy_chars = NULL,
        .persists_after_exec = false,
        .is_diagnostic = false,
    },
};

#define CHANNEL_COUNT (sizeof(channels) / sizeof(channels[0]))

/* One entry per live id: the table must describe every channel that is
 * not the retired slot 3. */
_Static_assert(
    CHANNEL_COUNT == PifkChannelIdCount - 1,
    "every channel id except the retired slot 3 needs a table entry");

/* ── Group metadata ──────────────────────────────────────────── */

static const char* const group_names[PifkGroupCount] = {
    [PifkGroupUsb] = "USB",
    [PifkGroupWireless] = "Wireless",
    [PifkGroupScreen] = "Screen",
    [PifkGroupWires] = "Wires",
};

/* Kept short enough to fit a submenu header: these are shown where the
 * operator is deciding whether they are in the right group at all. */
static const char* const group_preconditions[PifkGroupCount] = {
    [PifkGroupUsb] = "cable into a port",
    [PifkGroupWireless] = "get close to it",
    [PifkGroupScreen] = "a camera on us",
    [PifkGroupWires] = "header + shared GND",
};

const PifkChannel* pifk_channels(size_t* count) {
    if(count) *count = CHANNEL_COUNT;
    return channels;
}

const PifkChannel* pifk_channel_by_id(PifkChannelId id) {
    for(size_t i = 0; i < CHANNEL_COUNT; i++) {
        if(channels[i].id == id) return &channels[i];
    }
    return NULL; /* retired slot 3, or out of range */
}

const PifkChannel* pifk_channel_by_verb(const char* verb) {
    if(!verb) return NULL;
    for(size_t i = 0; i < CHANNEL_COUNT; i++) {
        if(channels[i].verb && strcasecmp(channels[i].verb, verb) == 0) {
            return &channels[i];
        }
    }
    return NULL;
}

const char* pifk_group_name(PifkChannelGroup group) {
    if(group >= PifkGroupCount) return "";
    return group_names[group];
}

const char* pifk_group_precondition(PifkChannelGroup group) {
    if(group >= PifkGroupCount) return "";
    return group_preconditions[group];
}

static const Icon* const group_icons[PifkGroupCount] = {
    [PifkGroupUsb] = &I_mg_usb_9x9,
    [PifkGroupWireless] = &I_mg_wireless_9x9,
    [PifkGroupScreen] = &I_mg_screen_9x9,
    [PifkGroupWires] = &I_mg_wires_9x9,
};

const Icon* pifk_group_icon(PifkChannelGroup group) {
    if(group >= PifkGroupCount) return NULL;
    return group_icons[group];
}

size_t pifk_group_channel_count(PifkChannelGroup group) {
    size_t n = 0;
    for(size_t i = 0; i < CHANNEL_COUNT; i++) {
        if(channels[i].group == group) n++;
    }
    return n;
}

/* ── Eligibility ─────────────────────────────────────────────── */

bool pifk_channel_accepts_text(const PifkChannel* ch, const char* text) {
    if(!ch || !text) return false;

    /* An empty payload has nothing to deliver on any channel, and every
     * executor already refuses it. Filter it here too so the list does
     * not offer a row that cannot work. */
    size_t len = strlen(text);
    if(len == 0) return false;

    if(len > ch->max_bytes) return false;

    /* Lossy characters are a rejection, not a warning: BadUSB skips
     * them silently, so the payload would arrive mangled while looking
     * correct on screen. */
    if(ch->count_lossy_chars && ch->count_lossy_chars(text) > 0) return false;

    return true;
}

const char* pifk_channel_reject_reason(const PifkChannel* ch, const char* text) {
    if(!ch || !text) return "unavailable";

    size_t len = strlen(text);
    if(len == 0) return "payload is empty";
    if(len > ch->max_bytes) return "too long";
    if(ch->count_lossy_chars && ch->count_lossy_chars(text) > 0) return "has untypable chars";

    return NULL;
}

size_t pifk_channel_eligible_count(const PifkChannel* ch, const void* payload_db) {
    const PayloadDb* db = payload_db;
    if(!ch || !db) return 0;

    size_t n = 0;
    for(uint16_t i = 0; i < db->payload_count; i++) {
        if(pifk_channel_accepts_text(ch, db->payloads[i].text)) n++;
    }
    return n;
}
