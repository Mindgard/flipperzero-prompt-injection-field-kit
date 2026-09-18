#include "payload_picker.h"
#include "pifk_icons.h"
#include "../payload/payload_db.h"
#include <string.h>

/* Labels must outlive the build call: the menu stores the pointer
 * rather than copying. */
static char labels[PIFK_MAX_PAYLOADS][PIFK_MAX_NAME_LEN + 3];

/* The caller's short-press handler, and the channel the current list was
 * filtered against.  File scope because the menu's callbacks cannot
 * carry extra context: their signature is fixed at (context, index), and
 * context is already the app.
 *
 * Only one payload list exists at a time — the menu is a single shared
 * view — so a single slot is sufficient. */
static PifkMenuCallback picker_on_select;
static const PifkChannel* picker_channel;

/* Category to glyph.
 *
 * Five families rather than one icon per category. There are twenty-one
 * categories, and at 9x9 the difference between "encoding" and
 * "obfuscation" cannot be drawn legibly — a mark the operator cannot
 * tell apart from its neighbour is worse than no mark, because it looks
 * like information.
 *
 * A starred payload shows a star instead: which payloads you have
 * chosen matters more, on the row, than which family they came from,
 * and the family is on the detail screen either way. */
static const Icon* payload_icon(const PifkPayload* p) {
    if(p->is_favorite) return &I_mg_pl_star_9x9;

    const char* c = p->category;
    if(!c[0]) return &I_mg_pl_direct_9x9;

    if(strcmp(c, "concealment") == 0 || strcmp(c, "obfuscation") == 0 ||
       strcmp(c, "indirect") == 0 || strcmp(c, "human-directed") == 0 ||
       strcmp(c, "authority-shed") == 0) {
        /* All read as innocuous content rather than as an instruction:
         * human-directed addresses the human reader and never names a
         * model, and authority-shed strips every role marker so the text
         * looks machine-written. */
        return &I_mg_pl_hidden_9x9;
    }
    if(strcmp(c, "encoding") == 0 || strcmp(c, "multilingual") == 0 ||
       strcmp(c, "delimiter") == 0) {
        return &I_mg_pl_encoded_9x9;
    }
    if(strcmp(c, "exfiltration") == 0 || strcmp(c, "tool-abuse") == 0) {
        return &I_mg_pl_exfil_9x9;
    }
    if(strcmp(c, "memory-poison") == 0 || strcmp(c, "crescendo") == 0) {
        return &I_mg_pl_persist_9x9;
    }
    /* instruction-override, jailbreak, impersonation, reasoning,
     * few-shot, qr, usbdesc, structural, authority: plainly-stated or
     * forged instructions, as distinct from text that reads as ordinary
     * content. structural and authority fall through deliberately -- a
     * forged log boundary or role marker is a fabricated instruction, not
     * a hidden one, so it belongs with impersonation. */
    return &I_mg_pl_direct_9x9;
}

/* Row position of each displayed payload's database index, filled by the
 * builder.  Needed because pifk_menu_set_selected_row() takes a row
 * position while our item ids are database indices — the two coincide
 * only for an unfiltered, unsorted list, which this never is, since
 * starred payloads are partitioned to the front. */
static uint16_t row_of_payload[PIFK_MAX_PAYLOADS];
static uint16_t row_count;

/* Rebuild the current list in place after a star toggle, so the row
 * moves to the top and the star appears without leaving the scene.
 * Declared before use; defined below the builder it calls. */
static void picker_rebuild(PifkApp* app);

/* Long press OK: toggle the star.  Short press is delivered straight to
 * the caller's own handler by the menu, so it needs no wrapper here. */
static void picker_long_cb(void* context, uint32_t index) {
    PifkApp* app = context;
    if(index >= app->payload_db->payload_count) return;

    payload_db_toggle_favorite(app->payload_db, (uint16_t)index);
    favorites_save(app->payload_db, PIFK_FAVORITES_FILE);

    /* Distinguishable from the success chirp an execute makes: this is a
     * state change, not a delivery. */
    notification_message(app->notifications, &sequence_blink_blue_100);

    /* Keep the cursor on the payload just starred, which has moved to
     * the top.  Its row position, not its database index — see
     * row_of_payload. */
    picker_rebuild(app);
    if(index < PIFK_MAX_PAYLOADS && row_of_payload[index] < row_count) {
        pifk_menu_set_selected_row(app->menu, row_of_payload[index]);
    }
}

/* Starred first, then original database order within each group.
 *
 * A stable partition rather than a sort: the database order is
 * meaningful (builtins are grouped by category) and an alphabetical
 * shuffle would lose that. */
static uint16_t picker_collect(PifkApp* app, const PifkChannel* ch, uint16_t* out) {
    uint16_t n = 0;

    for(int pass = 0; pass < 2; pass++) {
        bool want_favorite = (pass == 0);
        for(uint16_t i = 0; i < app->payload_db->payload_count && n < PIFK_MAX_PAYLOADS; i++) {
            const PifkPayload* p = &app->payload_db->payloads[i];
            if(p->is_favorite != want_favorite) continue;
            if(ch && !pifk_channel_accepts_text(ch, p->text)) continue;
            out[n++] = i;
        }
    }
    return n;
}

uint16_t
    pifk_payload_picker_build(PifkApp* app, const PifkChannel* ch, PifkMenuCallback on_select) {
    picker_on_select = on_select;
    picker_channel = ch;

    pifk_menu_set_callback(app->menu, on_select, app);
    pifk_menu_set_long_callback(app->menu, picker_long_cb);

    uint16_t order[PIFK_MAX_PAYLOADS];
    uint16_t n = picker_collect(app, ch, order);

    /* A payload absent from a filtered list has no row.  row_count
     * bounds the valid entries, so a stale value from a previous build
     * cannot be mistaken for a real position. */
    memset(row_of_payload, 0xFF, sizeof(row_of_payload));
    row_count = n;

    for(uint16_t row = 0; row < n; row++) {
        uint16_t idx = order[row];
        const PifkPayload* p = &app->payload_db->payloads[idx];

        /* No "* " prefix any more: the star is the icon. */
        snprintf(labels[row], sizeof(labels[row]), "%s", p->name);

        row_of_payload[idx] = row;

        /* The item index is the real database index, so callers index
         * payload_db directly rather than mapping back from a display
         * position — the mapping the old favorites scene got wrong. */
        pifk_menu_add_item(app->menu, labels[row], payload_icon(p), p->description, idx);
    }

    return n;
}

static void picker_rebuild(PifkApp* app) {
    pifk_menu_reset(app->menu);

    /* Header must outlive the call: the menu keeps the pointer. */
    static char header[64];
    pifk_payload_picker_header(app, picker_channel, header, sizeof(header));
    pifk_menu_set_header(app->menu, header);

    pifk_payload_picker_build(app, picker_channel, picker_on_select);
}

void pifk_payload_picker_header(PifkApp* app, const PifkChannel* ch, char* buf, size_t buf_size) {
    uint16_t total = app->payload_db->payload_count;

    if(!ch) {
        snprintf(buf, buf_size, "Payloads (%u)", total);
        return;
    }

    size_t fit = pifk_channel_eligible_count(ch, app->payload_db);

    /* Only say "of N" when something was actually hidden. "45 of 45"
     * spends header width to tell the operator nothing. */
    if(fit == total) {
        snprintf(buf, buf_size, "%s (%u)", ch->label, total);
    } else {
        snprintf(buf, buf_size, "%s - %u of %u fit", ch->label, (unsigned)fit, total);
    }
}
