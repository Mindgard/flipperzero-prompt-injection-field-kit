#pragma once

/*
 * Scrolling menu with per-row help.
 *
 * The stock Submenu module cannot do what this needs.  Left and Right go
 * unconsumed by its input handler and are then dropped by the
 * ViewDispatcher (only Back has a fallback), and there is no getter for
 * a View's existing input callback — view_i.h is not in the SDK — so a
 * Right-press handler cannot be added without replacing the scrolling
 * that makes the module work.
 *
 * This is written against the public GUI API (view_allocate_model,
 * canvas_*, elements_*) rather than adapted from the firmware's
 * submenu.c.  That is deliberate and not incidental: the firmware is
 * GPL-3.0 and this repository is Apache-2.0, and those are compatible
 * in one direction only.  Copying the stock module here would relicense
 * the app.  No firmware source was consulted while writing this file;
 * only the public headers were.
 *
 * Behaviour that matches the stock module, because operators expect it:
 *   - Up/Down move, wrapping at both ends
 *   - OK activates, and long-press OK is delivered separately
 *   - A scrollbar appears once the rows exceed the screen
 *
 * What it adds:
 *   - Right on any row with help text opens a scrollable help screen.
 *     Rows that have help are marked, so the affordance is discoverable
 *     rather than something the operator has to be told about.
 */

#include <furi.h>
#include <gui/view.h>
#include <gui/icon.h>

typedef struct PifkMenu PifkMenu;

/* Short press OK. */
typedef void (*PifkMenuCallback)(void* context, uint32_t index);

/* Long press OK.  Separate from the short-press callback so a caller
 * that wants only one does not have to filter InputType itself. */
typedef void (*PifkMenuLongCallback)(void* context, uint32_t index);

/* Right press on a row whose help text is non-NULL.  The caller renders
 * the help screen: this widget only reports the intent, because the
 * help view belongs to the scene (which owns the TextBox) rather than
 * to a menu. */
typedef void (*PifkMenuHelpCallback)(void* context, uint32_t index);

PifkMenu* pifk_menu_alloc(void);
void pifk_menu_free(PifkMenu* menu);
View* pifk_menu_get_view(PifkMenu* menu);

void pifk_menu_reset(PifkMenu* menu);

/* Header line, drawn inverted at the top.  Not copied — the pointer
 * must outlive the menu, same contract as the stock module. */
void pifk_menu_set_header(PifkMenu* menu, const char* header);

/* Add a row.
 *
 * `label` is not copied, matching the stock module's contract, because
 * every caller here already keeps its labels in static storage.
 *
 * `icon` is drawn at the start of the row and may be NULL, in which case
 * the label starts where the icon would have been — rows stay aligned
 * whether or not every one of them has a glyph.  Sized for 9x9; anything
 * taller is clipped by the 12px row.
 *
 * `help` may be NULL for a row with nothing to explain; that row shows
 * no marker and ignores Right.  Also not copied.
 *
 * `index` is the value passed to the callbacks and need not equal the
 * row position — the payload lists rely on this to pass a database
 * index straight through. */
void pifk_menu_add_item(
    PifkMenu* menu,
    const char* label,
    const Icon* icon,
    const char* help,
    uint32_t index);

void pifk_menu_set_callback(PifkMenu* menu, PifkMenuCallback cb, void* context);
void pifk_menu_set_long_callback(PifkMenu* menu, PifkMenuLongCallback cb);
void pifk_menu_set_help_callback(PifkMenu* menu, PifkMenuHelpCallback cb);

/* Position of the cursor, as a row number.  Distinct from the item
 * index a callback receives: with a filtered or reordered list the two
 * differ, which is a bug this codebase has already had once. */
uint16_t pifk_menu_get_selected_row(PifkMenu* menu);
void pifk_menu_set_selected_row(PifkMenu* menu, uint16_t row);

/* Help text of a row, or NULL.  Lets a scene render help for the
 * current row without keeping its own copy of the table. */
const char* pifk_menu_get_help(PifkMenu* menu, uint16_t row);
const char* pifk_menu_get_label(PifkMenu* menu, uint16_t row);
