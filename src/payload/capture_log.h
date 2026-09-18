#pragma once

#include "../pifk_app.h"

/* ── Capture log ─────────────────────────────────────────────── *
 *
 * Every other channel in this kit is write-only: it reports that a
 * payload was delivered, never whether it had any effect.  The GPIO
 * capture path already reads a target's reply off the wire, but until
 * now that reply lived only in a TextBox until the operator navigated
 * away.  This persists it, so a capture becomes evidence for a report
 * rather than something you had to photograph.
 *
 * Design notes worth knowing before changing this:
 *
 *   1. Append-only JSON Lines, not a JSON array.  Appending a record is
 *      one open-write-close with no read of prior content, so writing is
 *      O(1) in file size and needs no in-RAM copy of the log.  A
 *      half-written final line (battery pull mid-write) costs one record
 *      instead of corrupting the file.
 *
 *   2. Nothing is cached in PifkApp.  Records are read into a
 *      caller-owned array only while the viewer scene is on screen.  The
 *      predecessor of this feature was deleted for allocating ~8 KB of
 *      database at startup on a device with ~37 KB free; a fixed inline
 *      array here would repeat that.
 *
 *   3. The log is written by the device, read by the host.  The removed
 *      results viewer had this backwards — it displayed a results.json
 *      only the host tooling ever wrote, so the Flipper rendered a
 *      summary it did not produce.
 */

/* Cap on the log file.  At the limit the file is rotated to
 * captures.jsonl.1 (one generation, overwritten) and a fresh log
 * started, so the SD card cannot fill.
 *
 * This is independent of PIFK_MAX_FILE_SIZE: capture_read_file() applies
 * this cap itself rather than going through the shared whole-file reader.
 * It is deliberately *larger* than PIFK_MAX_FILE_SIZE, which is now an
 * 8 KB heap budget for the small config reads.
 *
 * KNOWN ISSUE: reading a full log still means a 32 KB heap buffer, and
 * capture_log_read_at() allocates one per record — so opening the viewer
 * on a full log does nine sequential 32 KB allocations on a device with
 * ~37 KB free.  Each is freed, so this is not a leak, but any one of them
 * can fail under fragmentation.  Fixing it properly means seeking to the
 * log's tail and reading a bounded window backwards, rather than reading
 * the file whole and walking it in RAM. */
#define PIFK_CAPTURE_MAX_BYTES (32 * 1024)

/* Reply bytes retained per record.  Matches PIFK_GPIO_RX_CAPACITY:
 * there is no point storing less of a reply than we captured. */
#define PIFK_CAPTURE_REPLY_LEN 512

/* Records the viewer reads at once.  Each is ~600 bytes, mostly reply
 * buffer, and the viewer holds them in .bss — so this is a fixed cost in
 * the FAP rather than heap competing with the radio and NFC stacks, but
 * a cost all the same.  Eight is more than fits legibly on a 128x64
 * screen; the header reports the true total from the file, so a small
 * window here does not hide records. */
#define PIFK_CAPTURE_VIEW_MAX 8

typedef struct {
    char payload[PIFK_MAX_NAME_LEN];
    char channel[16]; /* "gpio"; further capture-capable channels add their own */
    uint32_t ts; /* furi_get_tick() at capture; see the note in the .c */
    uint32_t sent; /* payload bytes transmitted */
    uint32_t rx; /* reply bytes captured; 0 is a finding, not an error */
    bool truncated; /* reply filled the capture buffer, target may have said more */
    char reply[PIFK_CAPTURE_REPLY_LEN];
} PifkCaptureRecord;

/* Append one record to the capture log, rotating first if the file has
 * reached PIFK_CAPTURE_MAX_BYTES.
 *
 * Returns false if the record could not be written.  Callers treat this
 * as non-fatal: failing to log a capture must not turn a successful
 * delivery into a reported failure, so the send result stands and the
 * logging failure is surfaced separately. */
bool capture_log_append(const PifkCaptureRecord* rec);

/* Build and append a record from a channel's raw capture results.
 *
 * Exists because the UI and the serial bridge both reach the same
 * capture path and must produce identical records — an operator
 * scripting from the host should leave the same evidence on the device
 * as one pressing buttons.  Timestamps with furi_get_tick() at call
 * time; `reply` may be NULL when nothing was captured. */
bool capture_log_record(
    const char* payload_name,
    const char* channel,
    size_t sent,
    const char* reply,
    size_t reply_len,
    bool truncated);

/* Read one record by age: index 0 is the most recent, 1 the one before
 * it, and so on.
 *
 * Returns false when `index` is past the oldest record, which is how a
 * caller walks the log without asking how long it is.  Reading one at a
 * time means only a single reply buffer needs to be live, which is why
 * the viewer holds summaries and calls this again for the body of
 * whichever record the operator selects.
 *
 * Malformed lines are skipped rather than aborting the read: this file is
 * user-visible on the SD card, and a hand-edited or partially-written
 * line should cost one record.  Skipped lines do not consume an index,
 * so indices always address real records. */
bool capture_log_read_at(PifkCaptureRecord* out, uint16_t index);

/* Total records currently in the log.  Cheap relative to reading them,
 * so the viewer can report "showing 16 of 40" without allocating for 40. */
uint16_t capture_log_count(void);

/* Delete the log and its rotated generation.  Exposed so an operator can
 * clear evidence between engagements without pulling the SD card. */
bool capture_log_clear(void);
