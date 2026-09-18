/*
 * Host-side tests for the streaming payloads.json object splitter.
 *
 * Build & run:
 *   cc -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -o /tmp/test_payload_stream tests/test_payload_stream.c
 *   /tmp/test_payload_stream
 *
 * Exits non-zero on failure so it can gate CI.
 *
 * Why this is worth testing: payloads.json used to be read whole into a
 * heap buffer during pifk_app_alloc(), which is what produced the
 * out-of-memory crash on launch.  It is now fed through a byte-at-a-time
 * state machine so memory use is fixed regardless of file size.
 *
 * That trade buys a new class of bug.  A whole-file parser sees every
 * object contiguously; a streaming one carries brace depth, string state
 * and escape state across read-chunk boundaries, and getting any of those
 * wrong corrupts or drops payloads.  The cases below are chosen for that:
 *
 *   - an object split across chunks must parse identically to one that
 *     is not, at every possible split point
 *   - '{' and '}' inside a JSON string are payload text, not structure.
 *     This is not hypothetical here: injection payloads regularly carry
 *     JSON and template braces
 *   - an escaped quote straddling a chunk boundary is what the `escaped`
 *     flag exists for
 *   - an object too large for the buffer must be skipped entirely, never
 *     truncated: a truncated payload would transmit text the operator
 *     did not author
 *
 * The splitter is duplicated from src/payload/payload_db.c rather than
 * linked, because that file needs the Flipper storage API. Keep
 * splitter_feed() and the buffer sizes in step with the originals.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PAYLOAD_OBJ_BUF_SIZE 1024

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

/* ── Mirrors PayloadSplitter in src/payload/payload_db.c ─────── */

typedef struct {
    char obj[PAYLOAD_OBJ_BUF_SIZE];
    size_t len;
    int depth;
    bool in_string;
    bool escaped;
    bool overflow;
    bool done;
} PayloadSplitter;

static bool splitter_feed(PayloadSplitter* s, char c) {
    if(s->depth == 0 && !s->in_string && c == ']') {
        s->done = true;
        return false;
    }

    if(s->depth > 0) {
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

/* ── Harness ─────────────────────────────────────────────────── */

#define MAX_OBJS 16

typedef struct {
    char obj[MAX_OBJS][PAYLOAD_OBJ_BUF_SIZE];
    int count;
} Collected;

/* Run `json` through the splitter in fixed-size chunks, mimicking the
 * storage_file_read() loop.  chunk_size of 0 means "all at once". */
static void split_all(const char* json, size_t chunk_size, Collected* out) {
    PayloadSplitter s;
    memset(&s, 0, sizeof(s));
    out->count = 0;

    size_t total = strlen(json);
    if(chunk_size == 0) chunk_size = total ? total : 1;

    bool saw_array = false;
    for(size_t base = 0; base < total; base += chunk_size) {
        size_t got = total - base;
        if(got > chunk_size) got = chunk_size;

        for(size_t i = 0; i < got; i++) {
            char c = json[base + i];
            if(!saw_array) {
                if(c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
                if(c != '[') return;
                saw_array = true;
                continue;
            }
            if(splitter_feed(&s, c) && out->count < MAX_OBJS) {
                memcpy(out->obj[out->count], s.obj, s.len + 1);
                out->count++;
            }
            if(s.done) return;
        }
    }
}

/* ── Tests ───────────────────────────────────────────────────── */

static void test_basic_objects(void) {
    puts("basic object extraction");
    Collected c;
    split_all("[{\"name\":\"a\"},{\"name\":\"b\"}]", 0, &c);
    CHECK(c.count == 2, "expected 2 objects, got %d", c.count);
    CHECK(strcmp(c.obj[0], "{\"name\":\"a\"}") == 0, "obj0 = %s", c.obj[0]);
    CHECK(strcmp(c.obj[1], "{\"name\":\"b\"}") == 0, "obj1 = %s", c.obj[1]);
}

static void test_non_array_rejected(void) {
    puts("a file that is not an array yields nothing");
    Collected c;
    split_all("{\"name\":\"a\"}", 0, &c);
    CHECK(c.count == 0, "expected 0 objects from a bare object, got %d", c.count);

    split_all("garbage", 0, &c);
    CHECK(c.count == 0, "expected 0 objects from garbage, got %d", c.count);

    /* Leading whitespace before '[' is allowed. */
    split_all("  \n\t [{\"name\":\"a\"}]", 0, &c);
    CHECK(c.count == 1, "expected 1 object past leading ws, got %d", c.count);
}

/* The core streaming property: the split point must not matter. */
static void test_every_split_point_agrees(void) {
    puts("every chunk boundary gives the same result");
    const char* json = "[{\"name\":\"first\",\"text\":\"hello world\"},"
                       "{\"name\":\"second\",\"text\":\"another one\"}]";

    Collected whole;
    split_all(json, 0, &whole);
    CHECK(whole.count == 2, "baseline expected 2, got %d", whole.count);

    for(size_t cs = 1; cs <= strlen(json); cs++) {
        Collected c;
        split_all(json, cs, &c);
        CHECK(c.count == whole.count, "chunk %zu: count %d != %d", cs, c.count, whole.count);
        for(int i = 0; i < c.count && i < whole.count; i++) {
            CHECK(
                strcmp(c.obj[i], whole.obj[i]) == 0,
                "chunk %zu obj%d: %s != %s",
                cs,
                i,
                c.obj[i],
                whole.obj[i]);
        }
    }
}

/* Braces inside a string are text. Injection payloads carry these. */
static void test_braces_inside_strings(void) {
    puts("braces and brackets inside strings are not structure");
    const char* json = "[{\"name\":\"tmpl\",\"text\":\"Reply with {\\\"ok\\\":true} now\"},"
                       "{\"name\":\"next\",\"text\":\"}}}{{{\"}]";

    /* Byte-at-a-time is the harshest schedule for the string state. */
    for(size_t cs = 1; cs <= 8; cs++) {
        Collected c;
        split_all(json, cs, &c);
        CHECK(c.count == 2, "chunk %zu: expected 2 objects, got %d", cs, c.count);
        if(c.count >= 1) {
            CHECK(
                strstr(c.obj[0], "{\\\"ok\\\":true}") != NULL,
                "chunk %zu: inner braces lost: %s",
                cs,
                c.obj[0]);
        }
        if(c.count >= 2) {
            CHECK(
                strstr(c.obj[1], "}}}{{{") != NULL, "chunk %zu: brace run lost: %s", cs, c.obj[1]);
        }
    }
}

/* An escaped quote split across a boundary is what `escaped` is for. */
static void test_escaped_quotes(void) {
    puts("escaped quotes survive chunk boundaries");
    const char* json = "[{\"name\":\"q\",\"text\":\"say \\\"hi\\\" then {stop}\"}]";

    for(size_t cs = 1; cs <= strlen(json); cs++) {
        Collected c;
        split_all(json, cs, &c);
        CHECK(c.count == 1, "chunk %zu: expected 1 object, got %d", cs, c.count);
        if(c.count == 1) {
            CHECK(
                strstr(c.obj[0], "{stop}") != NULL,
                "chunk %zu: trailing braces lost: %s",
                cs,
                c.obj[0]);
        }
    }
}

/* A trailing escape before the closing quote must not swallow it. */
static void test_escaped_backslash(void) {
    puts("an escaped backslash does not swallow the closing quote");
    const char* json = "[{\"name\":\"bs\",\"text\":\"ends with a backslash \\\\\"},"
                       "{\"name\":\"after\"}]";
    for(size_t cs = 1; cs <= 6; cs++) {
        Collected c;
        split_all(json, cs, &c);
        CHECK(c.count == 2, "chunk %zu: expected 2 objects, got %d", cs, c.count);
    }
}

static void test_nested_objects(void) {
    puts("nested objects close at the outer brace");
    Collected c;
    split_all("[{\"name\":\"n\",\"meta\":{\"a\":{\"b\":1}}},{\"name\":\"m\"}]", 0, &c);
    CHECK(c.count == 2, "expected 2 objects, got %d", c.count);
    CHECK(
        strcmp(c.obj[0], "{\"name\":\"n\",\"meta\":{\"a\":{\"b\":1}}}") == 0,
        "obj0 = %s",
        c.obj[0]);
}

/* Oversized objects must be dropped whole, never truncated: a truncated
 * payload would send text the operator never wrote. */
static void test_oversized_object_skipped(void) {
    puts("an object larger than the buffer is skipped, not truncated");

    char json[PAYLOAD_OBJ_BUF_SIZE * 3];
    size_t n = 0;
    n += (size_t)snprintf(json + n, sizeof(json) - n, "[{\"name\":\"big\",\"text\":\"");
    for(size_t i = 0; i < PAYLOAD_OBJ_BUF_SIZE + 200; i++)
        json[n++] = 'x';
    n += (size_t)snprintf(json + n, sizeof(json) - n, "\"},{\"name\":\"small\"}]");
    json[n] = '\0';

    for(size_t cs = 0; cs <= 64; cs += 16) {
        Collected c;
        split_all(json, cs, &c);
        CHECK(c.count == 1, "chunk %zu: expected only the small object, got %d", cs, c.count);
        if(c.count == 1) {
            CHECK(
                strcmp(c.obj[0], "{\"name\":\"small\"}") == 0,
                "chunk %zu: recovered wrong object: %s",
                cs,
                c.obj[0]);
        }
    }
}

/* A file cut off mid-object yields nothing for that object. */
static void test_truncated_file(void) {
    puts("an unterminated object is not emitted");
    Collected c;
    split_all("[{\"name\":\"a\"},{\"name\":\"trunc", 0, &c);
    CHECK(c.count == 1, "expected only the complete object, got %d", c.count);

    split_all("[{\"name\":\"only-open\"", 0, &c);
    CHECK(c.count == 0, "expected 0 objects, got %d", c.count);
}

static void test_empty_array(void) {
    puts("the first-run template stub yields no payloads");
    Collected c;
    /* This is what payload_db_write() emits when there is nothing to
     * export, trailing prose included. */
    split_all(
        "[\n]\n\nAdd payloads inside the brackets above, one object each:\n"
        "  {\"name\": \"my-payload\", \"text\": \"...\"}\n",
        0,
        &c);
    CHECK(c.count == 0, "template stub should yield 0 payloads, got %d", c.count);
}

/* Regression: the splitter must stop at the array's closing bracket.
 *
 * The whole-file parser stopped at ']' as a side effect of
 * json_next_object(); the streaming one has to do it explicitly.  Without
 * that, the worked {"name": ...} example in the prose after the bracket
 * loaded as a real payload, so a fresh install came up with a phantom
 * "my-payload" entry whose text was "...". */
static void test_stops_at_closing_bracket(void) {
    puts("nothing after the closing bracket is parsed");
    Collected c;
    split_all("[{\"name\":\"real\"}] {\"name\":\"ignored\"}", 0, &c);
    CHECK(c.count == 1, "expected 1 object, got %d", c.count);
    if(c.count >= 1) {
        CHECK(strcmp(c.obj[0], "{\"name\":\"real\"}") == 0, "obj0 = %s", c.obj[0]);
    }

    /* A ']' inside a string is text, not the end of the array. */
    split_all("[{\"name\":\"a\",\"text\":\"brackets ] here\"},{\"name\":\"b\"}]", 0, &c);
    CHECK(c.count == 2, "a ']' inside a string ended the array early: got %d", c.count);
}

int main(void) {
    puts("=== payload streaming splitter tests ===");
    test_basic_objects();
    test_non_array_rejected();
    test_every_split_point_agrees();
    test_braces_inside_strings();
    test_escaped_quotes();
    test_escaped_backslash();
    test_nested_objects();
    test_oversized_object_skipped();
    test_truncated_file();
    test_empty_array();
    test_stops_at_closing_bracket();

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
