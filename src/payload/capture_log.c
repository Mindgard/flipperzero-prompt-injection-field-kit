/*
 * Capture log — persisted evidence of what a target said back.
 *
 * See capture_log.h for the design rationale.  This file is the parsing
 * and I/O half, and two things in it need care rather than cleverness:
 *
 *   1. A captured reply is arbitrary bytes, not text.  It comes straight
 *      off a UART from hardware the kit knows nothing about, so it may
 *      contain NULs, control characters and bytes that are not valid
 *      UTF-8 in any encoding.  Everything written here therefore goes
 *      through capture_write_escaped(), which escapes >= 0x80 as well as
 *      the usual suspects.  Emitting a raw 0x80 byte would produce a
 *      file that host JSON parsers reject — which would make the
 *      evidence unreadable exactly when it matters.
 *
 *   2. Records are written by streaming, not by formatting a line into a
 *      buffer first: escaping a 512-byte reply can inflate it sixfold,
 *      and a stack buffer sized for that worst case would be larger than
 *      this app's whole thread stack allowance.  Reading takes the one
 *      allocation it needs, sized by the file's own cap.
 */

#include "capture_log.h"
#include <storage/storage.h>
#include <string.h>

/* ── Writing ─────────────────────────────────────────────────── */

/* Escape a byte range for JSON, treating the input as bytes rather than
 * text.  Differs from write_json_escaped() in payload_db.c only in that
 * bytes >= 0x80 are escaped too: that function's inputs are valid UTF-8
 * that must round-trip byte-identically (the homoglyph payloads depend
 * on it), whereas this one's input is whatever the target transmitted.
 *
 * Takes an explicit length because a reply may contain NUL bytes, so
 * strlen() would silently truncate the evidence. */
static void capture_write_escaped(File* file, const char* s, size_t len) {
    if(!s) return;
    for(size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if(c == '"') {
            storage_file_write(file, "\\\"", 2);
        } else if(c == '\\') {
            storage_file_write(file, "\\\\", 2);
        } else if(c == '\n') {
            storage_file_write(file, "\\n", 2);
        } else if(c == '\r') {
            storage_file_write(file, "\\r", 2);
        } else if(c == '\t') {
            storage_file_write(file, "\\t", 2);
        } else if(c < 0x20 || c >= 0x80) {
            char esc[7];
            int n = snprintf(esc, sizeof(esc), "\\u%04x", c);
            storage_file_write(file, esc, (size_t)n);
        } else {
            storage_file_write(file, &s[i], 1);
        }
    }
}

/* Rotate when the log has reached its cap.  One generation only: the
 * point is to bound disk use, and a numbered chain would just move the
 * unbounded growth somewhere else. */
static void capture_rotate_if_full(Storage* storage) {
    File* file = storage_file_alloc(storage);
    uint64_t size = 0;
    if(storage_file_open(file, PIFK_CAPTURES_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size = storage_file_size(file);
        storage_file_close(file);
    }
    storage_file_free(file);

    if(size < PIFK_CAPTURE_MAX_BYTES) return;

    /* storage_common_rename() will not overwrite, so clear the previous
     * generation first. */
    storage_simply_remove(storage, PIFK_CAPTURES_OLD_FILE);
    storage_common_rename(storage, PIFK_CAPTURES_FILE, PIFK_CAPTURES_OLD_FILE);
}

bool capture_log_append(const PifkCaptureRecord* rec) {
    if(!rec) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, PIFK_APP_DIR);
    capture_rotate_if_full(storage);

    File* file = storage_file_alloc(storage);
    /* OPEN_APPEND creates the file when absent, so first run needs no
     * special case. */
    if(!storage_file_open(file, PIFK_CAPTURES_FILE, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    char head[192];
    int n = snprintf(head, sizeof(head), "{\"ts\":%lu,\"channel\":\"", (unsigned long)rec->ts);
    storage_file_write(file, head, (size_t)n);
    capture_write_escaped(file, rec->channel, strlen(rec->channel));

    storage_file_write(file, "\",\"payload\":\"", 13);
    capture_write_escaped(file, rec->payload, strlen(rec->payload));

    n = snprintf(
        head,
        sizeof(head),
        "\",\"sent\":%lu,\"rx\":%lu,\"trunc\":%s,\"reply\":\"",
        (unsigned long)rec->sent,
        (unsigned long)rec->rx,
        rec->truncated ? "true" : "false");
    storage_file_write(file, head, (size_t)n);

    /* rx is the authority on reply length, not strlen: a reply may
     * legitimately contain NUL bytes.  Clamp to the buffer in case a
     * caller reports more than it stored. */
    size_t reply_len = rec->rx;
    if(reply_len > PIFK_CAPTURE_REPLY_LEN) reply_len = PIFK_CAPTURE_REPLY_LEN;
    capture_write_escaped(file, rec->reply, reply_len);

    storage_file_write(file, "\"}\n", 3);

    bool ok = !storage_file_get_error(file);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool capture_log_record(
    const char* payload_name,
    const char* channel,
    size_t sent,
    const char* reply,
    size_t reply_len,
    bool truncated) {
    PifkCaptureRecord rec;
    memset(&rec, 0, sizeof(rec));

    /* furi_get_tick(), not wall-clock: the Flipper has no reliably
     * synchronised RTC, and a confidently wrong timestamp in report
     * evidence is worse than an obviously relative one.  Ordering within
     * a session is what the viewer needs, and the file's line order
     * already provides that. */
    rec.ts = furi_get_tick();
    rec.sent = (uint32_t)sent;
    rec.truncated = truncated;

    if(payload_name) strlcpy(rec.payload, payload_name, sizeof(rec.payload));
    if(channel) strlcpy(rec.channel, channel, sizeof(rec.channel));

    if(reply && reply_len > 0) {
        if(reply_len > PIFK_CAPTURE_REPLY_LEN) reply_len = PIFK_CAPTURE_REPLY_LEN;
        /* memcpy, not strlcpy: a reply may contain NUL bytes and rx is
         * the authority on its length. */
        memcpy(rec.reply, reply, reply_len);
        rec.rx = (uint32_t)reply_len;
    }

    return capture_log_append(&rec);
}

/* ── Reading ─────────────────────────────────────────────────── */

/* Read the whole log into a malloc'd buffer.  Returns NULL when absent
 * or empty, which the callers treat as "no captures yet".
 *
 * Reads at most PIFK_CAPTURE_MAX_BYTES, so a log grown past the cap
 * by hand cannot make this allocation unbounded.  In that case the
 * oldest records are the ones dropped, since the parser walks from the
 * end — the newest captures are the ones an operator wants. */
static char* capture_read_file(size_t* out_len) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(!storage_file_open(file, PIFK_CAPTURES_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    uint64_t size = storage_file_size(file);
    if(size == 0) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }
    if(size > PIFK_CAPTURE_MAX_BYTES) size = PIFK_CAPTURE_MAX_BYTES;

    char* buf = malloc((size_t)size + 1);
    if(!buf) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    size_t total = 0;
    while(total < (size_t)size) {
        size_t want = (size_t)size - total;
        if(want > 4096) want = 4096;
        size_t got = storage_file_read(file, buf + total, want);
        if(got == 0) break; /* EOF or error */
        total += got;
    }
    buf[total] = '\0';

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(total == 0) {
        free(buf);
        return NULL;
    }
    if(out_len) *out_len = total;
    return buf;
}

/* ── Line-scoped JSON field extraction ───────────────────────── *
 *
 * The parser in payload_db.c scans within an object and is the right
 * tool for that file's arrays-of-objects.  Here each line is one object
 * and the fields are a known flat set, so a line-bounded scan is both
 * simpler and safer: a malformed line cannot bleed into the next record.
 */

/* Find "key": within [p, end).  Returns a pointer to the value or NULL.
 * Deliberately not escape-aware on the key side: keys are ours, always
 * plain identifiers, so a quote inside one is not a case that exists. */
static const char* line_find_key(const char* p, const char* end, const char* key) {
    size_t klen = strlen(key);
    /* Need at least "key": plus one value byte. */
    while(p + klen + 3 < end) {
        if(*p == '"' && (size_t)(end - p) > klen + 1 && memcmp(p + 1, key, klen) == 0 &&
           p[klen + 1] == '"') {
            const char* q = p + klen + 2;
            while(q < end && (*q == ' ' || *q == '\t'))
                q++;
            if(q < end && *q == ':') return q + 1;
        }
        p++;
    }
    return NULL;
}

/* Decode one \uXXXX escape.  Only the low byte is used: this parser reads
 * back what capture_write_escaped() produced, which never emits a code
 * point above 0xFF. */
static bool line_hex4(const char* p, const char* end, unsigned* out) {
    if(p + 4 > end) return false;
    unsigned v = 0;
    for(int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if(c >= '0' && c <= '9') {
            v |= (unsigned)(c - '0');
        } else if(c >= 'a' && c <= 'f') {
            v |= (unsigned)(c - 'a' + 10);
        } else if(c >= 'A' && c <= 'F') {
            v |= (unsigned)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    *out = v;
    return true;
}

/* Unescape a JSON string value into dst.  Returns the byte count
 * written, which is the authority on length since a decoded reply may
 * contain NULs.  dst is always NUL-terminated for the text views. */
static size_t line_extract_string(const char* p, const char* end, char* dst, size_t dst_len) {
    if(!dst || dst_len == 0) return 0;
    dst[0] = '\0';
    while(p < end && (*p == ' ' || *p == '\t'))
        p++;
    if(p >= end || *p != '"') return 0;
    p++;

    size_t i = 0;
    while(p < end && *p != '"') {
        char c = *p;
        if(c == '\\' && p + 1 < end) {
            p++;
            switch(*p) {
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u': {
                unsigned v = 0;
                if(line_hex4(p + 1, end, &v)) {
                    c = (char)(v & 0xFF);
                    p += 4;
                } else {
                    c = 'u';
                }
                break;
            }
            default:
                c = *p;
                break;
            }
        }
        if(i < dst_len - 1) dst[i++] = c;
        p++;
    }
    dst[i] = '\0';
    return i;
}

static uint32_t line_extract_uint(const char* p, const char* end) {
    while(p < end && (*p == ' ' || *p == '\t'))
        p++;
    uint64_t v = 0;
    while(p < end && *p >= '0' && *p <= '9') {
        if(v <= UINT32_MAX) v = v * 10 + (uint64_t)(*p - '0');
        p++;
    }
    return (v > UINT32_MAX) ? UINT32_MAX : (uint32_t)v;
}

/* Parse one line into a record.  Returns false if the line is not a
 * usable record, in which case the caller skips it: the log is visible
 * on the SD card and may have been hand-edited, and the last line of a
 * log interrupted mid-write is legitimately incomplete. */
static bool capture_parse_line(const char* start, const char* end, PifkCaptureRecord* rec) {
    memset(rec, 0, sizeof(*rec));

    const char* val = line_find_key(start, end, "payload");
    if(!val) return false;
    if(line_extract_string(val, end, rec->payload, sizeof(rec->payload)) == 0) return false;

    val = line_find_key(start, end, "channel");
    if(val) line_extract_string(val, end, rec->channel, sizeof(rec->channel));

    val = line_find_key(start, end, "ts");
    if(val) rec->ts = line_extract_uint(val, end);

    val = line_find_key(start, end, "sent");
    if(val) rec->sent = line_extract_uint(val, end);

    val = line_find_key(start, end, "trunc");
    if(val) {
        while(val < end && (*val == ' ' || *val == '\t'))
            val++;
        rec->truncated = (val + 4 <= end && memcmp(val, "true", 4) == 0);
    }

    /* reply last: rx is reset to the decoded length so the two cannot
     * disagree, even if a hand-edited line claims otherwise. */
    val = line_find_key(start, end, "reply");
    size_t reply_len = 0;
    if(val) reply_len = line_extract_string(val, end, rec->reply, sizeof(rec->reply));
    rec->rx = (uint32_t)reply_len;

    return true;
}

/* Walk parseable lines from the end of the buffer, newest first.
 *
 * With `out` non-NULL, stops once `want` records have been seen and
 * copies the last one out — so `want` = index+1 yields the record at
 * that age.  With `out` NULL it counts every parseable line.  Both modes
 * share this walk so a record the viewer can select is exactly a record
 * the count includes.
 *
 * Returns the number of parseable records seen. */
static uint16_t capture_walk_back(char* buf, size_t len, uint16_t want, PifkCaptureRecord* out) {
    uint16_t n = 0;
    const char* end = buf + len;

    /* Ignore a trailing newline so the last line is not seen as empty. */
    while(end > buf && (end[-1] == '\n' || end[-1] == '\r'))
        end--;

    while(end > buf) {
        const char* start = end;
        while(start > buf && start[-1] != '\n')
            start--;

        if(end > start) {
            PifkCaptureRecord scratch;
            /* Parse into scratch always: a malformed line must not
             * overwrite the caller's buffer, and skipped lines must not
             * consume an index. */
            if(capture_parse_line(start, end, &scratch)) {
                n++;
                if(out && n == want) {
                    memcpy(out, &scratch, sizeof(*out));
                    return n;
                }
            }
        }

        /* Step over the newline and any \r before it. */
        end = start;
        while(end > buf && (end[-1] == '\n' || end[-1] == '\r'))
            end--;
    }
    return n;
}

bool capture_log_read_at(PifkCaptureRecord* out, uint16_t index) {
    if(!out) return false;
    size_t len = 0;
    char* buf = capture_read_file(&len);
    if(!buf) return false;

    /* index is zero-based age; the walk counts from one. */
    uint16_t seen = capture_walk_back(buf, len, (uint16_t)(index + 1), out);
    free(buf);
    return seen == (uint16_t)(index + 1);
}

uint16_t capture_log_count(void) {
    size_t len = 0;
    char* buf = capture_read_file(&len);
    if(!buf) return 0;

    /* out NULL means "count everything". */
    uint16_t n = capture_walk_back(buf, len, 0, NULL);
    free(buf);
    return n;
}

bool capture_log_clear(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool ok = storage_simply_remove(storage, PIFK_CAPTURES_FILE);
    storage_simply_remove(storage, PIFK_CAPTURES_OLD_FILE);
    furi_record_close(RECORD_STORAGE);
    return ok;
}
