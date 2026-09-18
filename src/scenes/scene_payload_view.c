/*
 * Payload detail view — the payload-first path.
 *
 * ┌──────────────────────────┐
 * │ homoglyph-override       │
 * │ > View text              │
 * │   Deploy via...          │
 * └──────────────────────────┘
 *
 * Navigation is transport-first everywhere else, because the target
 * picks the channel. But there is one genuine payload-first workflow —
 * *this payload got a hit, try it everywhere else* — so "Deploy via..."
 * lists the channels that accept this payload. The same channel table,
 * queried the other way round.
 *
 * The channel list here names what was excluded and why, which the
 * transport-first path does not need to: an operator who chose the
 * payload deliberately is owed an explanation for a missing channel,
 * whereas one browsing a filtered payload list is not missing anything
 * they asked for.
 *
 * Starring moved to a long-press on any payload list (payload_picker.c),
 * so the Add/Remove Favorite row is gone.
 */

#include "../pifk_app.h"
#include "../channel/channel.h"
#include "../gui/pifk_menu.h"
#include "pifk_icons.h"
#include <string.h>

enum {
    PayloadViewIndexViewText = 0,
    PayloadViewIndexDeploy,
};

/* Scene state doubles as a mode flag, as it did before: 0 is the action
 * menu, 1 the text view, 2 the channel list. */
enum {
    PayloadViewModeMenu = 0,
    PayloadViewModeText,
    PayloadViewModeChannels,
};

#define PAYLOAD_VIEW_EVENT_SHOW_TEXT     0
#define PAYLOAD_VIEW_EVENT_SHOW_CHANNELS 1

/* Labels outlive the build: the menu stores the pointer. */
static char channel_rows[PifkChannelIdCount][40];

static void payload_view_menu_cb(void* context, uint32_t index);
static void payload_view_help_cb(void* context, uint32_t index);

static const char* const HELP_VIEW_TEXT = "The payload's full text,\n"
                                          "scrollable, with its\n"
                                          "category and description.\n\n"
                                          "Worth reading before you\n"
                                          "send it: what reaches the\n"
                                          "target is exactly these\n"
                                          "bytes.";

static const char* const HELP_DEPLOY = "The channels that can carry\n"
                                       "this payload, and why the\n"
                                       "others cannot.\n\n"
                                       "Navigation is transport-\n"
                                       "first everywhere else,\n"
                                       "because the target picks\n"
                                       "the channel. This is the\n"
                                       "other direction, for when\n"
                                       "one payload got a result\n"
                                       "and you want to try it\n"
                                       "elsewhere.";

static void payload_view_build_menu(PifkApp* app, const PifkPayload* p) {
    PifkMenu* menu = app->menu;
    pifk_menu_reset(menu);
    pifk_menu_set_callback(menu, payload_view_menu_cb, app);
    pifk_menu_set_help_callback(menu, payload_view_help_cb);
    pifk_menu_set_header(menu, p->name);
    pifk_menu_add_item(
        menu, "View text", &I_mg_pl_direct_9x9, HELP_VIEW_TEXT, PayloadViewIndexViewText);
    pifk_menu_add_item(menu, "Deploy via...", &I_mg_send_9x9, HELP_DEPLOY, PayloadViewIndexDeploy);
}

/* Right on a row shows its help.  Modes 1 and 2 both use the TextBox, so
 * the Back handler's existing "return to the action menu" branch already
 * covers returning from help. */
static void payload_view_help_cb(void* context, uint32_t index) {
    PifkApp* app = context;
    UNUSED(index);

    uint16_t row = pifk_menu_get_selected_row(app->menu);
    const char* help = pifk_menu_get_help(app->menu, row);
    const char* label = pifk_menu_get_label(app->menu, row);
    if(!help) return;

    snprintf(app->text_buf, sizeof(app->text_buf), "%s\n\n%s", label ? label : "", help);

    /* Mode Text, so Back rebuilds the action menu.  A help screen opened
     * from the channel list returns to the action menu rather than the
     * channel list, which is one press further out than ideal but never
     * strands the operator. */
    scene_manager_set_scene_state(app->scene_manager, PifkScenePayloadView, PayloadViewModeText);

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, app->text_buf);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
}

static void payload_view_channel_cb(void* context, uint32_t index) {
    PifkApp* app = context;
    app->selected_channel = (uint8_t)index;

    /* Reuse the transport-first execute path rather than duplicating
     * eleven executors. PayloadPick reads selected_payload_index, which
     * is already set. */
    scene_manager_next_scene(app->scene_manager, PifkScenePayloadPick);
}

/* Channels that accept this payload, then the ones that do not with the
 * reason. A rejected channel is shown rather than hidden because the
 * operator chose this payload on purpose and needs to know why the
 * channel they wanted is unavailable — that is the difference between
 * this screen and a filtered payload list. */
static void payload_view_build_channels(PifkApp* app, const PifkPayload* p) {
    PifkMenu* menu = app->menu;
    pifk_menu_reset(menu);
    pifk_menu_set_callback(menu, payload_view_channel_cb, app);
    pifk_menu_set_help_callback(menu, payload_view_help_cb);

    size_t count = 0;
    const PifkChannel* channels = pifk_channels(&count);

    size_t eligible = 0;
    for(size_t i = 0; i < count; i++) {
        if(pifk_channel_accepts_text(&channels[i], p->text)) eligible++;
    }

    static char header[40];
    snprintf(
        header, sizeof(header), "Deploy - %u of %u work", (unsigned)eligible, (unsigned)count);
    pifk_menu_set_header(menu, header);

    for(size_t i = 0; i < count; i++) {
        const PifkChannel* ch = &channels[i];
        if(!pifk_channel_accepts_text(ch, p->text)) continue;
        snprintf(
            channel_rows[ch->id],
            sizeof(channel_rows[ch->id]),
            "%s  (%s)",
            ch->label,
            pifk_group_name(ch->group));
        pifk_menu_add_item(menu, channel_rows[ch->id], ch->icon, ch->help, ch->id);
    }

    for(size_t i = 0; i < count; i++) {
        const PifkChannel* ch = &channels[i];
        const char* why = pifk_channel_reject_reason(ch, p->text);
        if(!why) continue;
        snprintf(channel_rows[ch->id], sizeof(channel_rows[ch->id]), "%s - %s", ch->label, why);
        /* Kept in the list rather than hidden: the operator chose this
         * payload on purpose and is owed the reason. Selecting it does
         * nothing, but Right still explains the channel. */
        pifk_menu_add_item(menu, channel_rows[ch->id], NULL, ch->help, ch->id);
    }
}

static void payload_view_menu_cb(void* context, uint32_t index) {
    PifkApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher,
        index == PayloadViewIndexViewText ? PAYLOAD_VIEW_EVENT_SHOW_TEXT :
                                            PAYLOAD_VIEW_EVENT_SHOW_CHANNELS);
}

void pifk_scene_payload_view_on_enter(void* context) {
    PifkApp* app = context;

    if(app->selected_payload_index >= app->payload_db->payload_count) {
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    const PifkPayload* p = &app->payload_db->payloads[app->selected_payload_index];

    payload_view_build_menu(app, p);

    scene_manager_set_scene_state(app->scene_manager, PifkScenePayloadView, PayloadViewModeMenu);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
}

bool pifk_scene_payload_view_on_event(void* context, SceneManagerEvent event) {
    PifkApp* app = context;
    const PifkPayload* p = &app->payload_db->payloads[app->selected_payload_index];

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == PAYLOAD_VIEW_EVENT_SHOW_TEXT) {
            /* No BadUSB warning here any more: a channel that cannot
             * carry this payload is not offered, so there is nothing to
             * warn about. The reason is on the Deploy screen, next to
             * the channel it applies to. */
            snprintf(
                app->text_buf,
                sizeof(app->text_buf),
                "%s\n[%s]\n%s\n%s",
                p->name,
                p->category[0] ? p->category : "uncategorized",
                p->description[0] ? p->description : "",
                p->text ? p->text : "(empty)");

            text_box_reset(app->text_box);
            text_box_set_text(app->text_box, app->text_buf);
            text_box_set_font(app->text_box, TextBoxFontText);
            view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
            scene_manager_set_scene_state(
                app->scene_manager, PifkScenePayloadView, PayloadViewModeText);
            return true;
        }
        if(event.event == PAYLOAD_VIEW_EVENT_SHOW_CHANNELS) {
            payload_view_build_channels(app, p);
            scene_manager_set_scene_state(
                app->scene_manager, PifkScenePayloadView, PayloadViewModeChannels);
            view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        uint32_t mode = scene_manager_get_scene_state(app->scene_manager, PifkScenePayloadView);
        if(mode != PayloadViewModeMenu) {
            /* Back returns to the action menu rather than leaving. */
            payload_view_build_menu(app, p);
            pifk_menu_set_selected_row(
                app->menu,
                mode == PayloadViewModeChannels ? PayloadViewIndexDeploy :
                                                  PayloadViewIndexViewText);
            scene_manager_set_scene_state(
                app->scene_manager, PifkScenePayloadView, PayloadViewModeMenu);
            view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
            return true;
        }
    }
    return false;
}

void pifk_scene_payload_view_on_exit(void* context) {
    PifkApp* app = context;
    pifk_menu_reset(app->menu);
    text_box_reset(app->text_box);
    scene_manager_set_scene_state(app->scene_manager, PifkScenePayloadView, PayloadViewModeMenu);
}
