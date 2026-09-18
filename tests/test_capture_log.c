/*
 * Host-side tests for the capture log's escaping and line parsing.
 *
 * The escape/parse logic is duplicated from src/payload/capture_log.c
 * rather than linked, because that file needs the Flipper storage API.
 * Keep the two in step: if capture_write_escaped(), line_find_key(),
 * line_extract_string(), line_extract_uint(), capture_parse_line() or
 * capture_walk_back() change there, change them here.
 *
 * The file writes are replaced by an in-memory sink; everything above
 * the I/O boundary is the real logic.
 *
 * Build & run:
 *   cc -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -o /tmp/test_capture_log tests/test_capture_log.c
 *   /tmp/test_capture_log
 *
 * Exits non-zero on failure so it can gate CI.
 *
 * What matters here: a captured reply is arbitrary bytes off a UART, so
 * the log must stay valid JSON whatever the target transmitted, and a
 * partially-written last line must cost one record rather than the file.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#define PIFK_MAX_NAME_LEN      48
#define PIFK_CAPTURE_REPLY_LEN 512

typedef struct {
    char payload[PIFK_MAX_NAME_LEN];
    char channel[16];
    uint32_t ts;
    uint32_t sent;
    uint32_t rx;
    bool truncated;
    char reply[PIFK_CAPTURE_REPLY_LEN];
} PifkCaptureRecord;

/* ── In-memory stand-in for File ─────────────────────────────── */

typedef struct {
    char buf[65536];
    size_t len;
} Sink;
static void sink_write(Sink* s, const void* data, size_t n) {
    if(s->len + n > sizeof(s->buf)) abort();
    memcpy(s->buf + s->len, data, n);
    s->len += n;
}
#define storage_file_write(f, d, n) sink_write((f), (d), (n))
#define File                        Sink

/* ── Mirror of capture_log.c ──────────────────────────────────── */

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

static void append_record(Sink* sink, const PifkCaptureRecord* rec) {
    char head[192];
    int n = snprintf(head, sizeof(head), "{\"ts\":%lu,\"channel\":\"", (unsigned long)rec->ts);
    storage_file_write(sink, head, (size_t)n);
    capture_write_escaped(sink, rec->channel, strlen(rec->channel));
    storage_file_write(sink, "\",\"payload\":\"", 13);
    capture_write_escaped(sink, rec->payload, strlen(rec->payload));
    n = snprintf(
        head,
        sizeof(head),
        "\",\"sent\":%lu,\"rx\":%lu,\"trunc\":%s,\"reply\":\"",
        (unsigned long)rec->sent,
        (unsigned long)rec->rx,
        rec->truncated ? "true" : "false");
    storage_file_write(sink, head, (size_t)n);
    size_t reply_len = rec->rx;
    if(reply_len > PIFK_CAPTURE_REPLY_LEN) reply_len = PIFK_CAPTURE_REPLY_LEN;
    capture_write_escaped(sink, rec->reply, reply_len);
    storage_file_write(sink, "\"}\n", 3);
}

static const char* line_find_key(const char* p, const char* end, const char* key) {
    size_t klen = strlen(key);
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
    val = line_find_key(start, end, "reply");
    size_t reply_len = 0;
    if(val) reply_len = line_extract_string(val, end, rec->reply, sizeof(rec->reply));
    rec->rx = (uint32_t)reply_len;
    return true;
}

static uint16_t capture_walk_back(char* buf, size_t len, uint16_t want, PifkCaptureRecord* out) {
    uint16_t n = 0;
    const char* end = buf + len;
    while(end > buf && (end[-1] == '\n' || end[-1] == '\r'))
        end--;
    while(end > buf) {
        const char* start = end;
        while(start > buf && start[-1] != '\n')
            start--;
        if(end > start) {
            PifkCaptureRecord scratch;
            if(capture_parse_line(start, end, &scratch)) {
                n++;
                if(out && n == want) {
                    memcpy(out, &scratch, sizeof(*out));
                    return n;
                }
            }
        }
        end = start;
        while(end > buf && (end[-1] == '\n' || end[-1] == '\r'))
            end--;
    }
    return n;
}

/* Mirror of capture_log_read_at(): index 0 is newest. */
static bool read_at(char* buf, size_t len, uint16_t index, PifkCaptureRecord* out) {
    uint16_t seen = capture_walk_back(buf, len, (uint16_t)(index + 1), out);
    return seen == (uint16_t)(index + 1);
}

/* Mirror of capture_log_count(). */
static uint16_t count_all(char* buf, size_t len) {
    return capture_walk_back(buf, len, 0, NULL);
}

/* ── Harness ─────────────────────────────────────────────────── */

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

/* Round-trip a record through write and parse. */
static void roundtrip(const PifkCaptureRecord* in, PifkCaptureRecord* out) {
    Sink s = {0};
    append_record(&s, in);
    CHECK(read_at(s.buf, s.len, 0, out), "expected to read back the record just written");
}

/* Every byte the writer emits must be printable ASCII JSON: that is the
 * whole point of escaping >= 0x80. */
static int all_json_safe(const Sink* s) {
    for(size_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->buf[i];
        if(c == '\n') continue;
        if(c < 0x20 || c >= 0x80) return 0;
    }
    return 1;
}

int main(void) {
    PifkCaptureRecord in, out;

    puts("test_basic_roundtrip");
    memset(&in, 0, sizeof(in));
    strcpy(in.payload, "qr-ignore");
    strcpy(in.channel, "gpio");
    in.ts = 128394;
    in.sent = 56;
    strcpy(in.reply, "You are a helpful assistant.");
    in.rx = (uint32_t)strlen(in.reply);
    roundtrip(&in, &out);
    CHECK(strcmp(out.payload, "qr-ignore") == 0, "payload '%s'", out.payload);
    CHECK(strcmp(out.channel, "gpio") == 0, "channel '%s'", out.channel);
    CHECK(out.ts == 128394, "ts %lu", (unsigned long)out.ts);
    CHECK(out.sent == 56, "sent %lu", (unsigned long)out.sent);
    CHECK(strcmp(out.reply, "You are a helpful assistant.") == 0, "reply '%s'", out.reply);
    CHECK(out.rx == in.rx, "rx %lu vs %lu", (unsigned long)out.rx, (unsigned long)in.rx);
    CHECK(out.truncated == false, "trunc should be false");
    printf("  reply round-trips: \"%s\"\n", out.reply);

    puts("\ntest_quotes_and_backslashes");
    memset(&in, 0, sizeof(in));
    strcpy(in.payload, "json-breakout");
    strcpy(in.channel, "gpio");
    strcpy(in.reply, "he said \"hi\" then C:\\path\\to");
    in.rx = (uint32_t)strlen(in.reply);
    roundtrip(&in, &out);
    CHECK(strcmp(out.reply, "he said \"hi\" then C:\\path\\to") == 0, "reply '%s'", out.reply);
    printf("  survives quotes and backslashes\n");

    puts("\ntest_newlines_tabs_controls");
    memset(&in, 0, sizeof(in));
    strcpy(in.payload, "multi");
    strcpy(in.channel, "gpio");
    memcpy(in.reply, "line1\nline2\r\n\ttab\x01\x1f", 19);
    in.rx = 19;
    Sink s1 = {0};
    append_record(&s1, &in);
    CHECK(all_json_safe(&s1), "control bytes must all be escaped");
    roundtrip(&in, &out);
    CHECK(out.rx == 19, "expected 19 bytes back, got %lu", (unsigned long)out.rx);
    CHECK(memcmp(out.reply, "line1\nline2\r\n\ttab\x01\x1f", 19) == 0, "control bytes differ");
    printf("  newline, CR, tab, 0x01 and 0x1f all round-trip\n");

    puts("\ntest_high_bytes_escaped_and_recovered");
    memset(&in, 0, sizeof(in));
    strcpy(in.payload, "homoglyph-override");
    strcpy(in.channel, "gpio");
    /* Cyrillic o in UTF-8 plus a lone invalid byte: a real UART can emit
     * either, and neither may reach the file raw. */
    memcpy(in.reply, "Ign\xd0\xbere\xff", 8);
    in.rx = 8;
    Sink s2 = {0};
    append_record(&s2, &in);
    CHECK(all_json_safe(&s2), "bytes >= 0x80 must be escaped, else invalid JSON");
    CHECK(strstr(s2.buf, "\\u00d0") != NULL, "0xd0 should appear as \\u00d0");
    CHECK(strstr(s2.buf, "\\u00ff") != NULL, "0xff should appear as \\u00ff");
    roundtrip(&in, &out);
    CHECK(out.rx == 8, "expected 8 bytes, got %lu", (unsigned long)out.rx);
    CHECK(memcmp(out.reply, "Ign\xd0\xbere\xff", 8) == 0, "high bytes must recover exactly");
    printf("  0xd0/0xff escaped as \\u00xx and recovered byte-exact\n");

    puts("\ntest_nul_bytes_in_reply");
    memset(&in, 0, sizeof(in));
    strcpy(in.payload, "nul");
    strcpy(in.channel, "gpio");
    memcpy(in.reply, "ab\0cd", 5);
    in.rx = 5; /* strlen would say 2 and lose the tail */
    roundtrip(&in, &out);
    CHECK(out.rx == 5, "expected 5 bytes, got %lu", (unsigned long)out.rx);
    CHECK(memcmp(out.reply, "ab\0cd", 5) == 0, "NUL-containing reply must survive");
    printf("  embedded NUL preserved; rx is the authority on length\n");

    puts("\ntest_truncated_flag");
    memset(&in, 0, sizeof(in));
    strcpy(in.payload, "big");
    strcpy(in.channel, "gpio");
    strcpy(in.reply, "partial");
    in.rx = 7;
    in.truncated = true;
    roundtrip(&in, &out);
    CHECK(out.truncated == true, "trunc should survive as true");
    printf("  trunc:true parsed back\n");

    puts("\ntest_silent_target_is_a_record");
    memset(&in, 0, sizeof(in));
    strcpy(in.payload, "b64-instruction");
    strcpy(in.channel, "gpio");
    in.sent = 134;
    in.rx = 0;
    roundtrip(&in, &out);
    CHECK(out.rx == 0, "rx should be 0");
    CHECK(out.sent == 134, "sent should still be recorded");
    CHECK(strcmp(out.payload, "b64-instruction") == 0, "payload name still present");
    printf("  a target that said nothing still yields a parseable record\n");

    puts("\ntest_newest_first_ordering");
    {
        Sink s = {0};
        const char* names[] = {"first", "second", "third"};
        for(int i = 0; i < 3; i++) {
            memset(&in, 0, sizeof(in));
            strcpy(in.payload, names[i]);
            strcpy(in.channel, "gpio");
            in.ts = (uint32_t)(1000 + i);
            append_record(&s, &in);
        }
        PifkCaptureRecord recs[3];
        for(int i = 0; i < 3; i++)
            CHECK(read_at(s.buf, s.len, (uint16_t)i, &recs[i]), "read index %d", i);
        CHECK(strcmp(recs[0].payload, "third") == 0, "newest first: got '%s'", recs[0].payload);
        CHECK(strcmp(recs[2].payload, "first") == 0, "oldest last: got '%s'", recs[2].payload);
        CHECK(count_all(s.buf, s.len) == 3, "count should be 3");
        /* One past the oldest must fail rather than return stale data:
         * that is how the viewer knows where to stop. */
        PifkCaptureRecord past;
        CHECK(!read_at(s.buf, s.len, 3, &past), "index 3 of 3 records must fail");
        printf(
            "  order: %s, %s, %s; index 3 correctly absent\n",
            recs[0].payload,
            recs[1].payload,
            recs[2].payload);
    }

    puts("\ntest_viewer_window_takes_newest");
    {
        /* The viewer reads the first PIFK_CAPTURE_VIEW_MAX indices;
         * with more in the file those must be the newest, and the count
         * must still report the true total so the header can say so. */
        Sink s = {0};
        for(int i = 0; i < 10; i++) {
            memset(&in, 0, sizeof(in));
            snprintf(in.payload, sizeof(in.payload), "p%d", i);
            strcpy(in.channel, "gpio");
            append_record(&s, &in);
        }
        PifkCaptureRecord recs[4];
        for(int i = 0; i < 4; i++)
            CHECK(read_at(s.buf, s.len, (uint16_t)i, &recs[i]), "read index %d", i);
        CHECK(
            strcmp(recs[0].payload, "p9") == 0, "newest should be p9, got '%s'", recs[0].payload);
        CHECK(strcmp(recs[3].payload, "p6") == 0, "4th newest p6, got '%s'", recs[3].payload);
        CHECK(count_all(s.buf, s.len) == 10, "total must still be 10");
        printf("  10 records, window of 4 -> p9..p6, count still 10\n");
    }

    puts("\ntest_count_walks_everything");
    {
        Sink s = {0};
        for(int i = 0; i < 7; i++) {
            memset(&in, 0, sizeof(in));
            snprintf(in.payload, sizeof(in.payload), "c%d", i);
            strcpy(in.channel, "gpio");
            append_record(&s, &in);
        }
        CHECK(count_all(s.buf, s.len) == 7, "count should be 7, got %u", count_all(s.buf, s.len));
        printf("  count walks all 7\n");
    }

    puts("\ntest_truncated_last_line_skipped");
    {
        /* A battery pull mid-write leaves a partial final line.  It must
         * cost that one record, not the whole file. */
        Sink s = {0};
        memset(&in, 0, sizeof(in));
        strcpy(in.payload, "good");
        strcpy(in.channel, "gpio");
        strcpy(in.reply, "ok");
        in.rx = 2;
        append_record(&s, &in);
        const char* partial = "{\"ts\":999,\"channel\":\"gp";
        sink_write(&s, partial, strlen(partial));

        PifkCaptureRecord rec;
        CHECK(count_all(s.buf, s.len) == 1, "expected 1 usable record");
        /* Index 0 must be the intact record, not the partial one: a
         * skipped line must not consume an index. */
        CHECK(read_at(s.buf, s.len, 0, &rec), "index 0 should resolve");
        CHECK(strcmp(rec.payload, "good") == 0, "got '%s'", rec.payload);
        printf("  partial final line skipped, prior record at index 0\n");
    }

    puts("\ntest_garbage_lines_skipped");
    {
        Sink s = {0};
        const char* junk = "not json at all\n{}\n[\n\n";
        sink_write(&s, junk, strlen(junk));
        memset(&in, 0, sizeof(in));
        strcpy(in.payload, "real");
        strcpy(in.channel, "gpio");
        append_record(&s, &in);
        const char* more = "###\n";
        sink_write(&s, more, strlen(more));

        PifkCaptureRecord rec;
        CHECK(count_all(s.buf, s.len) == 1, "only the real record should parse");
        CHECK(read_at(s.buf, s.len, 0, &rec), "index 0 should resolve past the junk");
        CHECK(strcmp(rec.payload, "real") == 0, "got '%s'", rec.payload);
        printf("  hand-edited junk skipped without aborting the read\n");
    }

    puts("\ntest_empty_log");
    {
        Sink s = {0};
        PifkCaptureRecord rec;
        CHECK(count_all(s.buf, 0) == 0, "empty buffer counts 0");
        CHECK(!read_at(s.buf, 0, 0, &rec), "empty buffer has no index 0");
        const char* nl = "\n\n\n";
        sink_write(&s, nl, 3);
        CHECK(count_all(s.buf, s.len) == 0, "blank lines count 0");
        CHECK(!read_at(s.buf, s.len, 0, &rec), "blank lines have no index 0");
        printf("  empty and blank-line-only logs yield no records\n");
    }

    puts("\ntest_max_length_reply_no_overflow");
    {
        /* Every byte high, so escaping inflates 6x: the widest case the
         * writer has to survive. */
        memset(&in, 0, sizeof(in));
        strcpy(in.payload, "maxlen");
        strcpy(in.channel, "gpio");
        memset(in.reply, '\xfe', PIFK_CAPTURE_REPLY_LEN);
        in.rx = PIFK_CAPTURE_REPLY_LEN;
        Sink s = {0};
        append_record(&s, &in);
        CHECK(all_json_safe(&s), "full high-byte reply stays JSON-safe");
        roundtrip(&in, &out);
        CHECK(
            out.rx == PIFK_CAPTURE_REPLY_LEN - 1,
            "parser keeps dst_len-1 bytes, got %lu",
            (unsigned long)out.rx);
        printf(
            "  512 high bytes -> %zu bytes on disk, recovered %lu\n",
            s.len,
            (unsigned long)out.rx);
    }

    puts("\ntest_rx_overclaim_is_clamped");
    {
        /* A caller reporting more than it stored must not read past the
         * buffer. ASan would catch it if it did. */
        memset(&in, 0, sizeof(in));
        strcpy(in.payload, "overclaim");
        strcpy(in.channel, "gpio");
        strcpy(in.reply, "short");
        in.rx = 99999;
        Sink s = {0};
        append_record(&s, &in);
        CHECK(all_json_safe(&s), "still JSON-safe");
        printf("  rx beyond the buffer clamped to %d\n", PIFK_CAPTURE_REPLY_LEN);
    }

    puts("\ntest_payload_name_needing_escape");
    {
        /* Names come from user payloads.json, so a quote is possible. */
        memset(&in, 0, sizeof(in));
        strcpy(in.payload, "odd\"name\\here");
        strcpy(in.channel, "gpio");
        roundtrip(&in, &out);
        CHECK(strcmp(out.payload, "odd\"name\\here") == 0, "got '%s'", out.payload);
        printf("  quoted/backslashed payload name round-trips\n");
    }

    printf("\n%d checks, %d failure(s)\n", checks, fails);
    return fails ? 1 : 0;
}
