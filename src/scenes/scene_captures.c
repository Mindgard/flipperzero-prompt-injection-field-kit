/*
 * Captures scene — what the target said back.
 *
 * ┌──────────────────────────┐
 * │ Captures (16 of 40)      │
 * │ ─────────────────────── │
 * │ > gpio qr-ignore 142B    │
 * │   gpio b64-instr silent  │
 * │   gpio homoglyph 88B     │
 * │   ...                    │
 * │   [Clear log]            │
 * └──────────────────────────┘
 *
 * Selecting a record shows the full reply in a scrolling TextBox, using
 * the same submenu-to-textbox toggle the payload view uses: the scene
 * records which of the two is showing in its scene state so Back returns
 * to the list rather than leaving the scene.
 *
 * The records are read into a static array owned by this file rather
 * than into PifkApp, and only while the scene is on screen. That is
 * deliberate. The feature this replaces kept its database in the app
 * struct and was deleted for costing ~8 KB of heap at startup on a
 * device reporting ~37 KB free; a viewer that only pays while it is
 * visible does not repeat that. Sixteen records is what fits usefully on
 * a 128x64 screen anyway, and the count in the header tells the operator
 * when there are more in the file.
 */

#include "../pifk_app.h"
#include "../gui/pifk_menu.h"
#include "pifk_icons.h"
#include "../payload/capture_log.h"
#include <string.h>

/* Toggle between the list and a single reply. Values are scene state, so
 * they persist across the view switch. */
#define CAPTURES_STATE_LIST 0
#define CAPTURES_STATE_TEXT 1

/* Reserved index for the Clear action, above any real record index. */
#define CAPTURES_INDEX_CLEAR 0xFF

#define CAPTURES_EVENT_SHOW_TEXT 0
#define CAPTURES_EVENT_CLEAR     1

/* Scene storage.  Deliberately split: the list needs only a one-line
 * summary per record, while the reply body is needed for exactly one
 * record at a time — the one being read.  Holding all eight replies
 * would cost ~4.8 KB of .bss to display ~600 bytes of it, so the list
 * keeps summaries and the reply is re-read from the file on selection.
 *
 * Static rather than malloc'd because scene on_exit does not run during
 * app teardown, so a heap buffer here would need a teardown hook too;
 * .bss is accounted at link time instead of competing with the runtime
 * heap the radio and NFC stacks draw from. */
typedef struct {
    char payload[PIFK_MAX_NAME_LEN];
    char channel[16];
    uint32_t ts;
    uint32_t sent;
    uint32_t rx;
    bool truncated;
} CaptureSummary;

static CaptureSummary captures[PIFK_CAPTURE_VIEW_MAX];
static uint16_t captures_shown;
static uint16_t captures_total;
static uint16_t captures_selected;

static void captures_callback(void* context, uint32_t index) {
    PifkApp* app = context;

    if(index == CAPTURES_INDEX_CLEAR) {
        view_dispatcher_send_custom_event(app->view_dispatcher, CAPTURES_EVENT_CLEAR);
        return;
    }
    if(index >= captures_shown) return;

    captures_selected = (uint16_t)index;
    scene_manager_set_scene_state(app->scene_manager, PifkSceneCaptures, CAPTURES_STATE_LIST);
    view_dispatcher_send_custom_event(app->view_dispatcher, CAPTURES_EVENT_SHOW_TEXT);
}

/* Labels must outlive on_enter: the menu stores pointers rather than
 * copying, and these were previously stack locals — which the stock
 * submenu tolerated only by luck. */
static char capture_labels[PIFK_CAPTURE_VIEW_MAX][48];

/* Index for the empty-state row, past every real capture index and the
 * Clear row. */
#define CAPTURES_INDEX_NONE 0xFFFE

static const char* const HELP_EMPTY = "Nothing captured yet.\n\n"
                                      "Captures come from one\n"
                                      "channel: Wires > Send +\n"
                                      "capture. It sends the\n"
                                      "payload on the UART TX pin\n"
                                      "and then listens on RX for\n"
                                      "whatever the target says\n"
                                      "back.\n\n"
                                      "Every other channel is\n"
                                      "write-only and has nothing\n"
                                      "to record.\n\n"
                                      "Needs TX, RX and a shared\n"
                                      "ground wired to the target.";

static const char* const HELP_CAPTURE_ROW = "A reply a target sent back.\n\n"
                                            "The row reads: channel,\n"
                                            "payload name, and how many\n"
                                            "bytes came back. A '+'\n"
                                            "means the reply was longer\n"
                                            "than the 512-byte buffer\n"
                                            "and was cut short.\n\n"
                                            "'silent' means the target\n"
                                            "said nothing. That is\n"
                                            "recorded rather than\n"
                                            "dropped, because it is a\n"
                                            "finding.\n\n"
                                            "OK shows the full reply.\n"
                                            "Everything here is also in\n"
                                            "captures.jsonl on the SD\n"
                                            "card, so the evidence\n"
                                            "survives this screen.";

static const char* const HELP_CLEAR = "Deletes captures.jsonl and\n"
                                      "its rotated predecessor.\n\n"
                                      "This is evidence. Copy the\n"
                                      "file off the SD card before\n"
                                      "clearing if the engagement\n"
                                      "is not written up yet.";

static void captures_help_cb(void* context, uint32_t index) {
    PifkApp* app = context;
    UNUSED(index);

    uint16_t row = pifk_menu_get_selected_row(app->menu);
    const char* help = pifk_menu_get_help(app->menu, row);
    const char* label = pifk_menu_get_label(app->menu, row);
    if(!help) return;

    snprintf(app->text_buf, sizeof(app->text_buf), "%s\n\n%s", label ? label : "", help);

    scene_manager_set_scene_state(app->scene_manager, PifkSceneCaptures, 1);
    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, app->text_buf);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
}

void pifk_scene_captures_on_enter(void* context) {
    PifkApp* app = context;
    PifkMenu* menu = app->menu;

    memset(captures, 0, sizeof(captures));
    captures_shown = 0;
    captures_total = capture_log_count();

    /* One record at a time, so only a single reply buffer is live rather
     * than PIFK_CAPTURE_VIEW_MAX of them.
     *
     * Each call re-reads and re-parses the whole log, so this is
     * quadratic in file size: eight passes over at most 32 KB, once on
     * scene entry. That is the price of the ~4 KB of .bss it saves, and
     * it is bounded by both the window and the log's own cap. If the
     * window ever grows much beyond eight, read once and walk instead.
     *
     * `rec` is ~600 bytes of a 4 KB thread stack (see application.fam),
     * which is why it is not two of them. */
    for(uint16_t i = 0; i < PIFK_CAPTURE_VIEW_MAX; i++) {
        PifkCaptureRecord rec;
        if(!capture_log_read_at(&rec, i)) break;
        CaptureSummary* s = &captures[captures_shown];
        strlcpy(s->payload, rec.payload, sizeof(s->payload));
        strlcpy(s->channel, rec.channel, sizeof(s->channel));
        s->ts = rec.ts;
        s->sent = rec.sent;
        s->rx = rec.rx;
        s->truncated = rec.truncated;
        captures_shown++;
    }

    pifk_menu_reset(menu);
    pifk_menu_set_callback(menu, captures_callback, app);
    pifk_menu_set_help_callback(menu, captures_help_cb);

    /* Header and labels must outlive this call: the menu stores the
     * pointers rather than copying. */
    static char header[32];
    if(captures_total > captures_shown) {
        snprintf(
            header,
            sizeof(header),
            "Captures (%u of %u)",
            (unsigned)captures_shown,
            (unsigned)captures_total);
    } else {
        snprintf(header, sizeof(header), "Captures (%u)", (unsigned)captures_shown);
    }
    pifk_menu_set_header(menu, header);

    if(captures_shown == 0) {
        /* One row that explains itself on Right, rather than four dead
         * rows spelling a sentence — that pattern read as a broken menu
         * where it was used before. */
        pifk_menu_add_item(
            menu, "(no captures yet)", &I_mg_captures_9x9, HELP_EMPTY, CAPTURES_INDEX_NONE);
    } else {
        for(uint16_t i = 0; i < captures_shown; i++) {
            const CaptureSummary* r = &captures[i];
            /* 6 channel + 20 name + " 4294967295B+" worst case, plus
             * separators and the NUL. */
            char* label = capture_labels[i];
            /* Clamp the name rather than the whole label: a payload name
             * can be 47 characters, and letting it run would push the
             * byte count off the end — the count is the more useful half,
             * and about 20 characters is what the screen fits anyway.
             *
             * A silent target is a result, so say so rather than showing
             * "0B" and letting it read as a failed capture. */
            if(r->rx == 0) {
                snprintf(
                    label, sizeof(capture_labels[i]), "%.6s %.20s silent", r->channel, r->payload);
            } else {
                snprintf(
                    label,
                    sizeof(capture_labels[i]),
                    "%.6s %.20s %luB%s",
                    r->channel,
                    r->payload,
                    (unsigned long)r->rx,
                    r->truncated ? "+" : "");
            }
            pifk_menu_add_item(menu, label, &I_mg_captures_9x9, HELP_CAPTURE_ROW, i);
        }
        pifk_menu_add_item(
            menu, "Clear log", &I_mg_diagnostic_9x9, HELP_CLEAR, CAPTURES_INDEX_CLEAR);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
}

bool pifk_scene_captures_on_event(void* context, SceneManagerEvent event) {
    PifkApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == CAPTURES_EVENT_SHOW_TEXT) {
            if(captures_selected >= captures_shown) return true;

            /* Re-read the selected record for its reply body. The list
             * holds summaries only, so this is where the reply enters
             * memory — one at a time, on a stack frame that goes away
             * when the operator navigates back. */
            PifkCaptureRecord rec;
            if(!capture_log_read_at(&rec, captures_selected)) {
                snprintf(
                    app->text_buf,
                    sizeof(app->text_buf),
                    "Could not re-read that\ncapture.\n\n"
                    "The log may have been\nedited or cleared while\nthis list was open.");
            } else {
                /* text_buf is 512 bytes and shared with 30 other call
                 * sites, while a reply can itself be 512 — so the header
                 * and the reply cannot both fit whole. Bound the reply
                 * with an explicit precision and tell the operator the
                 * full text is in captures.jsonl, rather than letting
                 * snprintf cut it silently.
                 *
                 * ts is a tick count, not wall-clock: the Flipper has no
                 * reliably synchronised RTC. Reported in seconds since
                 * boot so it is not mistaken for a real timestamp. */
                int n = snprintf(
                    app->text_buf,
                    sizeof(app->text_buf),
                    "%.32s / %.8s\n"
                    "sent %lu B, got %lu B%s\n"
                    "at %lus since boot\n"
                    "--- reply ---\n%.380s",
                    rec.payload,
                    rec.channel,
                    (unsigned long)rec.sent,
                    (unsigned long)rec.rx,
                    rec.truncated ? " (truncated)" : "",
                    (unsigned long)(rec.ts / 1000),
                    rec.rx > 0 ? rec.reply : "(target sent nothing)");

                if(n > 0 && rec.rx > 380) {
                    snprintf(
                        app->text_buf + n,
                        sizeof(app->text_buf) - (size_t)n,
                        "\n[+%lu B more in\n captures.jsonl]",
                        (unsigned long)(rec.rx - 380));
                }
            }

            text_box_reset(app->text_box);
            text_box_set_text(app->text_box, app->text_buf);
            text_box_set_font(app->text_box, TextBoxFontText);
            view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
            scene_manager_set_scene_state(
                app->scene_manager, PifkSceneCaptures, CAPTURES_STATE_TEXT);
            return true;
        }
        if(event.event == CAPTURES_EVENT_CLEAR) {
            capture_log_clear();
            /* Re-enter to redraw from the now-empty log. */
            scene_manager_previous_scene(app->scene_manager);
            scene_manager_next_scene(app->scene_manager, PifkSceneCaptures);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkSceneCaptures);
        if(state == CAPTURES_STATE_TEXT) {
            /* Back from a reply returns to the list, not out of the scene. */
            scene_manager_set_scene_state(
                app->scene_manager, PifkSceneCaptures, CAPTURES_STATE_LIST);
            view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
            return true;
        }
    }
    return false;
}

void pifk_scene_captures_on_exit(void* context) {
    PifkApp* app = context;
    pifk_menu_reset(app->menu);
    text_box_reset(app->text_box);
    scene_manager_set_scene_state(app->scene_manager, PifkSceneCaptures, CAPTURES_STATE_LIST);

    /* Summaries name the payloads used against a target, which is
     * engagement data; clear it rather than leaving it in .bss. The reply
     * bodies were never held here — see the storage note above. */
    memset(captures, 0, sizeof(captures));
    captures_shown = 0;
    captures_total = 0;
    captures_selected = 0;
}
