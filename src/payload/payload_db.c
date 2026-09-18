/*
 * Payload storage.
 *
 * Everything this module reads lives under /ext/apps_data/pifk:
 *   payloads.json       user payloads, merged over the compiled-in set
 *   favorites.json      names of starred payloads
 *   settings.json       delays, GPIO/UART parameters, I2C address
 *
 * Field names match the Python-side models so `hw flipper sync` can
 * write these files directly.
 *
 * Every byte here is untrusted: the files are user-editable on an SD
 * card, so the parser below treats malformed input as expected rather
 * than exceptional.  Strings are length-clamped, integers saturate, and
 * delays are capped — a corrupt file should mean a missing payload, not
 * a crash or a device that hangs for a week.
 */

#include "payload_db.h"
#include "../channel/channel.h"
#include "builtin_payloads.h"
#include "../execute/gpio_exec.h"
#include "../execute/i2c_exec.h"
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <string.h>

/* ── Tiny JSON helpers ───────────────────────────────────────── *
 *
 * The Flipper SDK has no JSON parser, so this is a hand-rolled scanner
 * for the flat arrays-of-objects we actually store.  It is not a
 * general JSON implementation and does not try to be:
 *
 *   - keys are found by linear scan within the enclosing object, so a
 *     nested object's keys are visible to the search
 *   - numbers are integers only; no floats, exponents or leading '+'
 *   - \uXXXX escapes are not decoded; the 'u' is taken literally
 *   - duplicate keys resolve to the first occurrence
 *
 * What it does guarantee is that malformed input cannot walk off the
 * end of the buffer.  Quote scanning is escape-aware and every
 * unterminated string aborts the search rather than advancing past the
 * NUL terminator.
 */

/* Skip whitespace, return pointer to next non-ws char. */
static const char* skip_ws(const char* p) {
    while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Extract a JSON string value after a key match.  Writes into dst
 * (max dst_len-1 chars) and returns pointer past the closing quote.
 * Returns NULL on parse error. */
static const char* json_extract_string(const char* p, char* dst, size_t dst_len) {
    if(!dst || dst_len == 0) return NULL; /* dst_len-1 would underflow */
    p = skip_ws(p);
    if(*p != '"') return NULL;
    p++;
    size_t i = 0;
    while(*p && *p != '"') {
        if(*p == '\\' && *(p + 1)) {
            p++;
            switch(*p) {
            case 'n':
                if(i < dst_len - 1) dst[i++] = '\n';
                break;
            case 't':
                if(i < dst_len - 1) dst[i++] = '\t';
                break;
            case '"':
                if(i < dst_len - 1) dst[i++] = '"';
                break;
            case '\\':
                if(i < dst_len - 1) dst[i++] = '\\';
                break;
            default:
                if(i < dst_len - 1) dst[i++] = *p;
                break;
            }
        } else {
            if(i < dst_len - 1) dst[i++] = *p;
        }
        p++;
    }
    dst[i] = '\0';
    if(*p == '"') p++;
    return p;
}

/* Find "key": in the current JSON object scope.
 * Returns pointer to the value (after the colon) or NULL.
 *
 * Escape sequences are honoured when scanning for a closing quote: a
 * value containing \" would otherwise desynchronise the quote pairing
 * and make every subsequent key in the object unfindable. */
static const char* json_find_key(const char* p, const char* key) {
    size_t klen = strlen(key);
    while(*p) {
        p = skip_ws(p);
        if(*p == '}') return NULL; /* end of object */
        if(*p == '"') {
            const char* start = p + 1;
            const char* end = start;
            bool escaped = false;
            while(*end) {
                if(escaped) {
                    escaped = false;
                } else if(*end == '\\') {
                    escaped = true;
                } else if(*end == '"') {
                    break;
                }
                end++;
            }
            /* Unterminated string: `end` is at the NUL, so advancing
             * past it would read beyond the buffer.  Bail out. */
            if(*end != '"') return NULL;
            if((size_t)(end - start) == klen && memcmp(start, key, klen) == 0) {
                p = end + 1;
                p = skip_ws(p);
                if(*p == ':') return p + 1;
            }
            p = end + 1;
        } else {
            p++;
        }
    }
    return NULL;
}

/* Extract a JSON integer value.  Returns pointer past the number.
 *
 * Accumulates in int64_t and saturates: overflowing an int32_t here
 * would be undefined behaviour, and the FAP is not built with -fwrapv. */
static const char* json_extract_int(const char* p, int32_t* out) {
    p = skip_ws(p);
    int64_t val = 0;
    bool neg = false;
    if(*p == '-') {
        neg = true;
        p++;
    }
    while(*p >= '0' && *p <= '9') {
        if(val <= (int64_t)INT32_MAX) {
            val = val * 10 + (*p - '0');
        }
        p++;
    }
    if(val > (int64_t)INT32_MAX) val = (int64_t)INT32_MAX;
    *out = neg ? (int32_t)(-val) : (int32_t)val;
    return p;
}

/* Object boundary scanning used to live here, as json_next_object() and
 * json_object_end() over a whole-file buffer.  It is now incremental, in
 * PayloadSplitter below, so payloads.json never has to be resident. */

/* ── JSON output escaping ────────────────────────────────────── */

/* Write a JSON-escaped string to file.  Every string field written to
 * disk must go through this: names, categories and descriptions all
 * originate from user JSON or the serial bridge, so a stray quote or
 * backslash would otherwise emit a file we can no longer parse.
 *
 * Bytes >= 0x80 pass through unchanged, so text that arrived as valid
 * UTF-8 round-trips byte-identically — which the homoglyph and
 * zero-width payloads depend on.  Callers writing arbitrary bytes rather
 * than text (a captured UART reply, say) need to escape those
 * themselves; see capture_log.c. */
void write_json_escaped(File* file, const char* s) {
    if(!s) return;
    while(*s) {
        unsigned char c = (unsigned char)*s;
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
        } else if(c < 0x20) {
            char esc[7];
            int n = snprintf(esc, sizeof(esc), "\\u%04x", c);
            storage_file_write(file, esc, (uint16_t)n);
        } else {
            storage_file_write(file, s, 1);
        }
        s++;
    }
}

/* ── File I/O ────────────────────────────────────────────────── */

/* Read entire file into a malloc'd buffer.  Returns NULL on failure. */
static char* read_entire_file(const char* path, size_t* out_len) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    /* Sanity limit.  storage_file_read() takes and returns uint16_t, so
     * the cap must stay below 65536: a file of exactly 65536 bytes would
     * truncate to a read length of 0 and yield a silently empty buffer. */
    uint64_t size = storage_file_size(file);
    if(size == 0 || size > PIFK_MAX_FILE_SIZE) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    char* buf = malloc((size_t)size + 1);
    if(!buf) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    /* Loop: a single storage_file_read() is not guaranteed to return
     * the whole file. */
    size_t total = 0;
    while(total < (size_t)size) {
        size_t want = (size_t)size - total;
        if(want > 4096) want = 4096;
        uint16_t got = storage_file_read(file, buf + total, (uint16_t)want);
        if(got == 0) break; /* EOF or error */
        total += got;
    }
    buf[total] = '\0';

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(out_len) *out_len = total;
    return buf;
}

/* ── Payload DB ──────────────────────────────────────────────── */

void payload_db_load_builtins(PayloadDb* db) {
    for(uint16_t i = 0; i < BUILTIN_PAYLOAD_COUNT && db->payload_count < PIFK_MAX_PAYLOADS; i++) {
        memcpy(&db->payloads[db->payload_count], &BUILTIN_PAYLOADS[i], sizeof(PifkPayload));
        db->payloads[db->payload_count].is_builtin = true;
        db->payload_count++;
    }
}

/* ── Streaming payload load ──────────────────────────────────── *
 *
 * payloads.json is parsed one object at a time rather than read whole.
 * The old whole-file read cost a heap buffer the size of the file at the
 * worst possible moment — during pifk_app_alloc(), with the GUI and the
 * ~12 KB database already resident — and that was the OOM-on-launch.
 *
 * Streaming makes the cost fixed regardless of file size: one small read
 * window plus one object buffer, both stack-allocated below.
 *
 * A payload object's extractable content is bounded by the field widths
 * (name 48 + text 512 + category 32 + description 128 ≈ 720 bytes), so a
 * 1 KB object buffer holds every well-formed entry.  Anything longer is
 * skipped rather than truncated: a half-parsed payload would deliver
 * different text than the file specifies, which for this app means
 * sending something the operator did not intend.
 */

#define PAYLOAD_OBJ_BUF_SIZE 1024
#define PAYLOAD_READ_CHUNK   256

/* Parse one complete JSON object's text and add it to the database.
 * `obj` is NUL-terminated and spans '{' .. '}' inclusive.
 *
 * Kept free of Storage so it can be exercised directly by the host-side
 * tests; see tests/test_payload_stream.c. */
static void payload_add_from_object(PayloadDb* db, const char* obj) {
    if(db->payload_count >= PIFK_MAX_PAYLOADS) return;

    PifkPayload pl;
    memset(&pl, 0, sizeof(pl));
    pl.is_builtin = false;

    const char* val;
    char tmp[PIFK_MAX_TEXT_LEN];

    val = json_find_key(obj, "name");
    if(val) json_extract_string(val, pl.name, sizeof(pl.name));

    val = json_find_key(obj, "text");
    tmp[0] = '\0';
    if(val) json_extract_string(val, tmp, sizeof(tmp));

    val = json_find_key(obj, "category");
    if(val) json_extract_string(val, pl.category, sizeof(pl.category));

    val = json_find_key(obj, "description");
    if(val) json_extract_string(val, pl.description, sizeof(pl.description));

    /* Skip if name or text is empty, or the name is already taken —
     * payload_db_find() returns the first match, so a duplicate would
     * occupy a slot no lookup could ever reach.  This is also what makes
     * a builtin's name unusable from the file. */
    if(pl.name[0] == '\0' || tmp[0] == '\0') return;
    if(payload_db_find(db, pl.name)) return;

    char* dup = strdup(tmp);
    if(!dup) return;

    pl.text = dup;
    pl.text_owned = true;
    memcpy(&db->payloads[db->payload_count], &pl, sizeof(PifkPayload));
    db->payload_count++;
}

/* Incremental object splitter.
 *
 * Feeding the file through this a chunk at a time means brace depth,
 * string state and escape state all have to survive a chunk boundary —
 * hence the struct rather than locals.  Braces and brackets inside a
 * JSON string are literal text, so the string state is what keeps a
 * payload containing '{' from being read as structure. */
typedef struct {
    char obj[PAYLOAD_OBJ_BUF_SIZE];
    size_t len; /* bytes buffered in obj */
    int depth; /* brace nesting; 0 = between objects */
    bool in_string;
    bool escaped;
    bool overflow; /* current object outgrew obj; skip it */
    bool done; /* closing ']' seen; stop feeding */
} PayloadSplitter;

/* Feed one byte.  Returns true when obj holds a complete, NUL-terminated
 * object ready for payload_add_from_object().
 *
 * Sets s->done when the array's closing ']' is reached, which the caller
 * must honour: payload_db_write() emits explanatory prose after the
 * bracket, and that prose contains a worked {"name": ...} example.  The
 * whole-file parser this replaced stopped at ']' as a side effect of
 * json_next_object(); here it has to be explicit, or the example in the
 * first-run template file gets loaded as a real payload. */
static bool splitter_feed(PayloadSplitter* s, char c) {
    if(s->depth == 0 && !s->in_string && c == ']') {
        s->done = true;
        return false;
    }

    if(s->depth > 0) {
        /* Buffer first, so the closing brace lands in obj too. */
        if(s->len + 1 < sizeof(s->obj)) {
            s->obj[s->len++] = c;
        } else {
            s->overflow = true;
        }
    }

    if(s->in_string) {
        if(s->escaped) {
            s->escaped = false;
        } else if(c == '\\') {
            s->escaped = true;
        } else if(c == '"') {
            s->in_string = false;
        }
        return false;
    }

    if(c == '"') {
        s->in_string = true;
        return false;
    }

    if(c == '{') {
        if(s->depth == 0) {
            /* Start of a new object: reset the buffer and capture the
             * brace the depth check above skipped. */
            s->len = 0;
            s->overflow = false;
            s->obj[s->len++] = c;
        }
        s->depth++;
        return false;
    }

    if(c == '}') {
        if(s->depth > 0) {
            s->depth--;
            if(s->depth == 0) {
                if(s->overflow) {
                    s->len = 0;
                    s->overflow = false;
                    return false;
                }
                s->obj[s->len] = '\0';
                return true;
            }
        }
        return false;
    }

    return false;
}

void payload_db_load_from_file(PayloadDb* db, const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return;
    }

    /* No file-size cap needed: memory use here does not scale with it. */
    PayloadSplitter s;
    memset(&s, 0, sizeof(s));

    char chunk[PAYLOAD_READ_CHUNK];
    bool saw_array = false;

    while(db->payload_count < PIFK_MAX_PAYLOADS) {
        uint16_t got = storage_file_read(file, chunk, sizeof(chunk));
        if(got == 0) break; /* EOF or error */

        for(uint16_t i = 0; i < got; i++) {
            /* Require a leading '[' before any object, matching the old
             * parser's rejection of a file that is not an array. */
            if(!saw_array) {
                const char c = chunk[i];
                if(c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
                if(c != '[') goto done;
                saw_array = true;
                continue;
            }

            if(splitter_feed(&s, chunk[i])) {
                payload_add_from_object(db, s.obj);
                if(db->payload_count >= PIFK_MAX_PAYLOADS) goto done;
            }
            if(s.done) goto done;
        }
    }

done:
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* Schema stub written when there is nothing user-authored to export.
 *
 * The point of the first-run file is to show the operator the field names
 * so they can add their own payloads.  It is emphatically NOT a dump of
 * the builtin set: every builtin already lives in flash, and
 * payload_db_load_from_file() rejects all of them by name on the way back
 * in, so writing them costs a file-sized heap buffer at every subsequent
 * launch and buys nothing.  See the header note on PIFK_MAX_PAYLOADS for
 * how little heap there is to spend. */
/* The trailing prose sits after the closing bracket on purpose: the
 * object loop in payload_db_load_from_file() stops at ']', so these bytes
 * are never parsed.  This scanner has no comment support — do not move
 * the text inside the array expecting it to be skipped. */
static const char PAYLOAD_TEMPLATE[] =
    "[\n"
    "]\n"
    "\n"
    "Add payloads inside the brackets above, one object each:\n"
    "  {\"name\": \"my-payload\", \"text\": \"...\",\n"
    "   \"category\": \"instruction-override\", \"description\": \"...\"}\n"
    "Separate objects with commas.  \"name\" and \"text\" are required;\n"
    "a name matching a built-in payload is ignored, so pick your own.\n";

/* Write user-authored payloads as JSON, to seed an editable template on
 * first run.  Builtins are deliberately excluded — see PAYLOAD_TEMPLATE.
 *
 * Returns false only on an I/O failure; a database with no user payloads
 * writes the schema stub and succeeds. */
static bool payload_db_write(const PayloadDb* db, const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, PIFK_APP_DIR);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    uint16_t user_count = 0;
    for(uint16_t i = 0; i < db->payload_count; i++) {
        if(!db->payloads[i].is_builtin) user_count++;
    }

    if(user_count == 0) {
        storage_file_write(file, PAYLOAD_TEMPLATE, sizeof(PAYLOAD_TEMPLATE) - 1);
    } else {
        storage_file_write(file, "[\n", 2);
        bool first = true;

        for(uint16_t i = 0; i < db->payload_count; i++) {
            if(db->payloads[i].is_builtin) continue;
            if(!first) storage_file_write(file, ",\n", 2);
            first = false;

            storage_file_write(file, "  {\"name\": \"", 12);
            write_json_escaped(file, db->payloads[i].name);
            storage_file_write(file, "\", \"text\": \"", 12);
            write_json_escaped(file, db->payloads[i].text);
            storage_file_write(file, "\", \"category\": \"", 16);
            write_json_escaped(file, db->payloads[i].category);
            storage_file_write(file, "\", \"description\": \"", 19);
            write_json_escaped(file, db->payloads[i].description);
            storage_file_write(file, "\"}", 2);
        }

        storage_file_write(file, "\n]\n", 3);
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return true;
}

bool payload_db_export_user(const PayloadDb* db, const char* path) {
    return payload_db_write(db, path);
}

const PifkPayload* payload_db_find(const PayloadDb* db, const char* name) {
    for(uint16_t i = 0; i < db->payload_count; i++) {
        if(strcmp(db->payloads[i].name, name) == 0) {
            return &db->payloads[i];
        }
    }
    return NULL;
}

void payload_db_toggle_favorite(PayloadDb* db, uint16_t index) {
    if(index < db->payload_count) {
        db->payloads[index].is_favorite = !db->payloads[index].is_favorite;
    }
}

/* ── Favorites persistence ────────────────────────────────────── */

bool favorites_save(const PayloadDb* db, const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, PIFK_APP_DIR);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    storage_file_write(file, "[", 1);
    bool first = true;
    for(uint16_t i = 0; i < db->payload_count; i++) {
        if(!db->payloads[i].is_favorite) continue;
        if(!first) storage_file_write(file, ",", 1);
        first = false;
        storage_file_write(file, "\"", 1);
        write_json_escaped(file, db->payloads[i].name);
        storage_file_write(file, "\"", 1);
    }
    storage_file_write(file, "]", 1);

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return true;
}

void favorites_load(PayloadDb* db, const char* path) {
    size_t len = 0;
    char* json = read_entire_file(path, &len);
    if(!json) return;

    const char* p = json;
    p = skip_ws(p);
    if(*p != '[') {
        free(json);
        return;
    }
    p++;

    char name_buf[PIFK_MAX_NAME_LEN];
    while(*p) {
        p = skip_ws(p);
        if(*p == ']') break;
        if(*p == '"') {
            p = json_extract_string(p, name_buf, sizeof(name_buf));
            if(!p) break;
            /* Find payload by name and mark as favorite */
            for(uint16_t i = 0; i < db->payload_count; i++) {
                if(strcmp(db->payloads[i].name, name_buf) == 0) {
                    db->payloads[i].is_favorite = true;
                    break;
                }
            }
        } else if(*p == ',') {
            p++;
        } else {
            p++;
        }
    }

    free(json);
}

/* ── Reload ──────────────────────────────────────────────────── */

void pifk_reload_data(PifkApp* app) {
    /* Free existing user data */
    payload_db_free_entries(app->payload_db);

    /* Zero the whole DB, not just the counts.  Leaving stale
     * text_owned/turns_owned flags and dangling pointers in slots above
     * the new count is how a reload turns into a double free. */
    memset(app->payload_db, 0, sizeof(PayloadDb));

    /* Reload everything */
    payload_db_load_builtins(app->payload_db);
    payload_db_load_from_file(app->payload_db, PIFK_PAYLOADS_FILE);
    favorites_load(app->payload_db, PIFK_FAVORITES_FILE);
    settings_load(app, PIFK_SETTINGS_FILE);
}

/* ── Settings persistence ─────────────────────────────────────── */

bool settings_save(const PifkApp* app, const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, PIFK_APP_DIR);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    char buf[320];
    int n = snprintf(
        buf,
        sizeof(buf),
        "{\"badusb_delay_ms\":%lu,"
        "\"gpio_baud\":%lu,"
        "\"gpio_serial_id\":%u,"
        "\"gpio_line_ending\":%u,"
        "\"gpio_byte_delay_ms\":%lu,"
        "\"gpio_listen_ms\":%lu,"
        "\"i2c_address\":%u}",
        (unsigned long)app->badusb_delay_ms,
        (unsigned long)app->gpio_baud,
        (unsigned)app->gpio_serial_id,
        (unsigned)app->gpio_line_ending,
        (unsigned long)app->gpio_byte_delay_ms,
        (unsigned long)app->gpio_listen_ms,
        (unsigned)app->i2c_address);
    storage_file_write(file, buf, (uint16_t)n);

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return true;
}

void settings_load(PifkApp* app, const char* path) {
    size_t len = 0;
    char* json = read_entire_file(path, &len);
    if(!json) return;

    const char* val;
    int32_t tmp;

    /* Delays are clamped to PIFK_MAX_DELAY_MS.  An unbounded value
     * here becomes an uninterruptible furi_delay_ms() — a corrupt or
     * hostile settings.json could otherwise hang the app for days. */
    val = json_find_key(json, "badusb_delay_ms");
    if(val) {
        json_extract_int(val, &tmp);
        if(tmp > 0) {
            if(tmp > PIFK_MAX_DELAY_MS) tmp = PIFK_MAX_DELAY_MS;
            app->badusb_delay_ms = (uint32_t)tmp;
        }
    }

    /* convo_delay_ms, default_protocol and default_channel are all
     * retired.  An older settings.json still containing those keys is
     * simply ignored, which is what the defensive parser already does
     * is simply ignored, which is what the defensive parser already does
     * for anything it does not recognise. */

    /* GPIO settings.  Every value is range-checked: this file is
     * user-editable, and an out-of-range baud or serial id would be
     * passed straight into the HAL. */
    val = json_find_key(json, "gpio_baud");
    if(val) {
        json_extract_int(val, &tmp);
        if(tmp >= 300 && tmp <= 921600) app->gpio_baud = (uint32_t)tmp;
    }

    val = json_find_key(json, "gpio_serial_id");
    if(val) {
        json_extract_int(val, &tmp);
        if(tmp >= 0 && tmp < (int32_t)FuriHalSerialIdMax) {
            app->gpio_serial_id = (uint8_t)tmp;
        }
    }

    val = json_find_key(json, "gpio_line_ending");
    if(val) {
        json_extract_int(val, &tmp);
        if(tmp >= 0 && tmp < (int32_t)PifkGpioLineEndingCount) {
            app->gpio_line_ending = (uint8_t)tmp;
        }
    }

    val = json_find_key(json, "gpio_listen_ms");
    if(val) {
        json_extract_int(val, &tmp);
        if(tmp < 0) tmp = 0;
        if(tmp > PIFK_MAX_DELAY_MS) tmp = PIFK_MAX_DELAY_MS;
        app->gpio_listen_ms = (uint32_t)tmp;
    }

    val = json_find_key(json, "gpio_byte_delay_ms");
    if(val) {
        json_extract_int(val, &tmp);
        if(tmp < 0) tmp = 0;
        if(tmp > PIFK_GPIO_MAX_BYTE_DELAY_MS) tmp = PIFK_GPIO_MAX_BYTE_DELAY_MS;
        app->gpio_byte_delay_ms = (uint32_t)tmp;
    }

    /* Out-of-range addresses are rejected rather than clamped: clamping
     * 0x00 up to 0x08 would silently retarget a write at a device the
     * user did not name. */
    val = json_find_key(json, "i2c_address");
    if(val) {
        json_extract_int(val, &tmp);
        if(tmp >= PIFK_I2C_ADDR_MIN && tmp <= PIFK_I2C_ADDR_MAX) {
            app->i2c_address = (uint8_t)tmp;
        }
    }

    free(json);
}

/* ── Cleanup ─────────────────────────────────────────────────── */

/* Both of these clear the ownership flag as well as the pointer, so a
 * second call is a no-op rather than a double free. */

void payload_db_free_entries(PayloadDb* db) {
    for(uint16_t i = 0; i < PIFK_MAX_PAYLOADS; i++) {
        if(db->payloads[i].text_owned && db->payloads[i].text) {
            free((void*)db->payloads[i].text);
        }
        db->payloads[i].text = NULL;
        db->payloads[i].text_owned = false;
    }
}
