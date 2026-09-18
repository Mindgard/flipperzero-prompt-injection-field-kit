/*
 * Host-side tests for the payload picker's ordering and row mapping.
 *
 * Build & run:
 *   cc -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -o /tmp/test_payload_picker tests/test_payload_picker.c
 *   /tmp/test_payload_picker
 *
 * Exits non-zero on failure so it can gate CI.
 *
 * Why this is worth testing: the picker deliberately uses the payload
 * *database index* as the submenu item id, so scenes can index the
 * database directly instead of mapping back from a display position —
 * the mapping the old favorites scene had a comment apologising for.
 * The cost is that two different indices are now in play, and
 * submenu_set_selected_item() wants the other one: a row position.
 *
 * They coincide only for an unfiltered list with nothing starred, which
 * is exactly the case that would be tested by hand, and never the case
 * in the field. Starring payload 30 on the QR list (18 of 45 eligible)
 * would otherwise move the cursor to row 30 of an 18-row menu.
 *
 * The logic is duplicated from src/scenes/payload_picker.c rather than
 * linked, because that file needs the Flipper GUI. Keep picker_collect()
 * and the row_of_payload fill in step with the originals.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PIFK_MAX_PAYLOADS 56
#define ROW_NONE          0xFFFF

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

typedef struct {
    const char* text;
    bool is_favorite;
} Payload;

/* Mirrors picker_collect(): starred first, database order within each
 * group, filtered by capacity. max_bytes of 0 means "no filter". */
static uint16_t collect(const Payload* db, uint16_t count, size_t max_bytes, uint16_t* out) {
    uint16_t n = 0;
    for(int pass = 0; pass < 2; pass++) {
        bool want_favorite = (pass == 0);
        for(uint16_t i = 0; i < count && n < PIFK_MAX_PAYLOADS; i++) {
            if(db[i].is_favorite != want_favorite) continue;
            if(max_bytes && strlen(db[i].text) > max_bytes) continue;
            out[n++] = i;
        }
    }
    return n;
}

/* Mirrors the row_of_payload fill in pifk_payload_picker_build(). */
static uint16_t
    build_rows(const Payload* db, uint16_t count, size_t max_bytes, uint16_t* row_of_payload) {
    uint16_t order[PIFK_MAX_PAYLOADS];
    uint16_t n = collect(db, count, max_bytes, order);

    for(uint16_t i = 0; i < PIFK_MAX_PAYLOADS; i++) {
        row_of_payload[i] = ROW_NONE;
    }
    for(uint16_t row = 0; row < n; row++) {
        row_of_payload[order[row]] = row;
    }
    return n;
}

int main(void) {
    uint16_t order[PIFK_MAX_PAYLOADS];
    uint16_t rows[PIFK_MAX_PAYLOADS];

    puts("payload picker ordering");

    /* ── The sentinel must not be a usable row position ──
     *
     * Asserted against the row bound rather than against ROW_NONE
     * itself. Every "has no row" check below compares to ROW_NONE, so if
     * the sentinel were 0 both sides of those comparisons would move
     * together and stay true while the behaviour became wrong: a
     * filtered-out payload would claim row 0 and starring an invisible
     * payload would drag the cursor to the top of the list. Found by
     * mutation testing — the checks below all passed with ROW_NONE
     * redefined to 0. */
    CHECK(
        ROW_NONE >= PIFK_MAX_PAYLOADS,
        "ROW_NONE (%u) must be outside every valid row position",
        (unsigned)ROW_NONE);

    /* ── Starred payloads sort to the front, order preserved ── */
    {
        const Payload db[] = {
            {"aaa", false}, /* 0 */
            {"bbb", true}, /* 1 */
            {"ccc", false}, /* 2 */
            {"ddd", true}, /* 3 */
        };
        uint16_t n = collect(db, 4, 0, order);
        CHECK(n == 4, "all four listed, got %u", n);
        CHECK(order[0] == 1, "first row is the first starred, got %u", order[0]);
        CHECK(order[1] == 3, "second row is the second starred, got %u", order[1]);
        CHECK(order[2] == 0, "then unstarred in database order, got %u", order[2]);
        CHECK(order[3] == 2, "then unstarred in database order, got %u", order[3]);
    }

    /* ── Database order within a group is stable, not alphabetical ──
     *
     * The builtin set is grouped by category, and an alphabetical sort
     * would scatter it. */
    {
        const Payload db[] = {
            {"zzz", false},
            {"aaa", false},
            {"mmm", false},
        };
        uint16_t n = collect(db, 3, 0, order);
        CHECK(n == 3, "three listed");
        CHECK(
            order[0] == 0 && order[1] == 1 && order[2] == 2,
            "database order preserved, got %u %u %u",
            order[0],
            order[1],
            order[2]);
    }

    /* ── The bug this file exists for ──
     *
     * A filtered list where the item id and the row position diverge.
     * Payload 3 is starred and eligible, so it is row 0 — passing its
     * database index to submenu_set_selected_item() would land on row 3,
     * which here is past the end of the menu. */
    {
        const Payload db[] = {
            {"aaaaaaaaaa", false}, /* 0: 10 bytes, too long */
            {"bbbbbbbbbb", false}, /* 1: 10 bytes, too long */
            {"cccccccccc", false}, /* 2: 10 bytes, too long */
            {"dd", true}, /* 3: 2 bytes, starred, fits */
            {"ee", false}, /* 4: 2 bytes, fits */
        };
        uint16_t n = build_rows(db, 5, 5, rows);
        CHECK(n == 2, "only two payloads fit a 5-byte channel, got %u", n);
        CHECK(rows[3] == 0, "starred eligible payload is row 0, got %u", rows[3]);
        CHECK(rows[4] == 1, "other eligible payload is row 1, got %u", rows[4]);
        CHECK(rows[3] != 3, "row position must not equal the database index here");

        /* Filtered-out payloads have no row, and must be distinguishable
         * from row 0 — otherwise the cursor jumps to the top when the
         * operator stars something invisible. */
        CHECK(rows[0] == ROW_NONE, "filtered-out payload has no row, got %u", rows[0]);
        CHECK(rows[1] == ROW_NONE, "filtered-out payload has no row");
        CHECK(rows[2] == ROW_NONE, "filtered-out payload has no row");
    }

    /* ── Unfiltered, nothing starred: the indices coincide ──
     *
     * The case that hides the bug. Asserted so a future change that
     * breaks it is visible rather than surprising. */
    {
        const Payload db[] = {{"a", false}, {"b", false}, {"c", false}};
        uint16_t n = build_rows(db, 3, 0, rows);
        CHECK(n == 3, "three rows");
        CHECK(
            rows[0] == 0 && rows[1] == 1 && rows[2] == 2,
            "row == index when unfiltered and unstarred");
    }

    /* ── Starring changes row positions ──
     *
     * The reason the cursor has to be repositioned after a rebuild at
     * all: the starred payload moves to the front. */
    {
        Payload db[] = {{"a", false}, {"b", false}, {"c", false}};
        build_rows(db, 3, 0, rows);
        CHECK(rows[2] == 2, "payload 2 starts at row 2");

        db[2].is_favorite = true; /* long-press OK */
        build_rows(db, 3, 0, rows);
        CHECK(rows[2] == 0, "after starring, payload 2 is row 0, got %u", rows[2]);
        CHECK(rows[0] == 1, "payload 0 shifted down to row 1, got %u", rows[0]);
    }

    /* ── Empty list ──
     *
     * Reachable with a payloads.json whose every entry exceeds the
     * channel. The scene shows an explanation instead of an empty menu,
     * so the count has to be honestly zero. */
    {
        const Payload db[] = {{"aaaaaaaaaa", false}};
        uint16_t n = build_rows(db, 1, 5, rows);
        CHECK(n == 0, "nothing fits, got %u", n);
        CHECK(rows[0] == ROW_NONE, "the excluded payload has no row");
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
