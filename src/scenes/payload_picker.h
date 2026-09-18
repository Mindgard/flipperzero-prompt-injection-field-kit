#pragma once

/*
 * Shared payload list builder.
 *
 * Payload lists existed in three places, each with its own copy of the
 * list-and-index logic: the payload browser, the favorites scene, and
 * the Quick Deploy picker in settings.  Filtering has to apply to all of
 * them, so the builder is shared rather than copied.
 *
 * Two behaviours every payload list now gets:
 *
 *   - Starred payloads sort to the top, prefixed "*".  That is where
 *     they are useful, and it is why the separate favorites scene went
 *     away.
 *   - Long-press OK toggles the star, via the menu's long callback.
 */

#include "../pifk_app.h"
#include "../channel/channel.h"
#include "../gui/pifk_menu.h"

/* Build a payload list into app->submenu.
 *
 * `ch` filters the list to payloads that channel can carry; NULL lists
 * everything.  The submenu item index is always the real payload_db
 * index, so callers never map back through a display position — the
 * bug the old favorites scene worked around with a comment.
 *
 * `on_select` fires on a short OK press.  Starring is handled here.
 *
 * Returns the number of rows added.  Zero means the caller should show
 * an empty state rather than an empty menu: a submenu with no items
 * gives no feedback at all, and the old favorites scene faked it with
 * three rows that had NULL callbacks and read as broken.
 */
uint16_t
    pifk_payload_picker_build(PifkApp* app, const PifkChannel* ch, PifkMenuCallback on_select);

/* Header text for a filtered list: "Type it · 43 of 45 fit".
 *
 * An operator not told that payloads were filtered out will conclude
 * the library is small, so the count is not optional.  Writes into
 * `buf` because submenu_set_header does not copy.
 */
void pifk_payload_picker_header(PifkApp* app, const PifkChannel* ch, char* buf, size_t buf_size);
