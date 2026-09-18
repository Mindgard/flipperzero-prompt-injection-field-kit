/*
 * Main menu scene — top-level navigation, grouped by transport.
 *
 * ┌──────────────────────────┐
 * │      Pifk - PIFK     │
 * │ > USB          (2)       │
 * │   Wireless     (5)       │
 * │   Screen       (1)       │
 * │   Wires        (3)       │
 * │   Payloads (45)          │
 * │   Captures               │
 * │   Remote Mode            │
 * │   Settings               │
 * │   About                  │
 * └──────────────────────────┘
 *
 * Transport comes first because it is the axis the operator does not
 * choose: the target picks it. A kiosk with an exposed port is BadUSB, a
 * tablet behind glass is QR, an HMI with a debug header is UART. The
 * payload is the free variable, and it is picked second.
 *
 * It is also the scarce axis. QR carries 18 of the 45 shipped payloads;
 * every other channel carries at least 43. Choosing the payload first is
 * what produced the four "too long" rejection dialogs this replaces —
 * choose the channel first and the payload list simply shows what fits.
 * See docs/ui-transport-first.md.
 *
 * The group rows are indices 0-3 rather than named constants offset into
 * a legacy range: unlike the old menu, this scene's saved state is reset
 * on upgrade (the rows changed meaning, so preserving the index would
 * land the cursor somewhere arbitrary), which is the one migration cost
 * taken deliberately.
 *
 * Payloads stays as a browsable library. Reading the set to learn what
 * prompt injection looks like is payload-first by nature and a
 * legitimate use of the kit.
 */

#include "../pifk_app.h"
#include "../channel/channel.h"
#include "../gui/pifk_menu.h"
#include "pifk_icons.h"

/* Help for the rows that are not transport groups.  The groups get
 * theirs from pifk_group_precondition() plus the channel table. */
static const char* const HELP_PAYLOADS = "The whole payload library,\n"
                                         "unfiltered.\n\n"
                                         "Everywhere else the list is\n"
                                         "narrowed to what the chosen\n"
                                         "channel can carry. This one\n"
                                         "shows all of them, because\n"
                                         "reading the set to learn\n"
                                         "what prompt injection looks\n"
                                         "like is a legitimate use of\n"
                                         "the kit.\n\n"
                                         "Open one and use 'Deploy\n"
                                         "via...' to see which\n"
                                         "channels accept it, and why\n"
                                         "the others do not.\n\n"
                                         "Long-press OK on any row to\n"
                                         "star it. Starred payloads\n"
                                         "sort to the top of every\n"
                                         "list.";

static const char* const HELP_CAPTURES = "Replies the targets sent\n"
                                         "back, newest first.\n\n"
                                         "Delivery is not effect.\n"
                                         "Eleven of the twelve\n"
                                         "channels are write-only and\n"
                                         "can only tell you a payload\n"
                                         "left the device. Send +\n"
                                         "capture, on the Wires\n"
                                         "group, reads the target's\n"
                                         "reply off the wire, and\n"
                                         "every one lands here.\n\n"
                                         "A target that stayed silent\n"
                                         "is recorded too: 'it said\n"
                                         "nothing' is a finding, not\n"
                                         "a failed capture.\n\n"
                                         "Written to captures.jsonl\n"
                                         "so the evidence survives\n"
                                         "leaving this screen.";

static const char* const HELP_REMOTE = "Drives the kit from a host\n"
                                       "over USB serial, so payload\n"
                                       "delivery can be scripted.\n\n"
                                       "A line protocol on CDC\n"
                                       "channel 1: PING, LIST,\n"
                                       "EXEC <channel> <payload>,\n"
                                       "STATUS, LOAD, RELOAD.\n\n"
                                       "LOAD pushes a one-off\n"
                                       "payload without touching\n"
                                       "the SD card, which is the\n"
                                       "fast path when iterating on\n"
                                       "wording against a live\n"
                                       "target.\n\n"
                                       "Captures driven from the\n"
                                       "host are logged on the\n"
                                       "device identically, so\n"
                                       "scripting still leaves\n"
                                       "evidence here.";

static const char* const HELP_SETTINGS = "Parameters, not actions.\n\n"
                                         "BadUSB start delay, the\n"
                                         "UART's baud, port, line\n"
                                         "ending, pacing and listen\n"
                                         "window, and the I2C\n"
                                         "address.\n\n"
                                         "Pin numbers are shown\n"
                                         "read-only and come from the\n"
                                         "firmware at runtime, so\n"
                                         "they are right for your\n"
                                         "build whatever a diagram\n"
                                         "says.\n\n"
                                         "Every change is written to\n"
                                         "settings.json immediately -\n"
                                         "there is no save step.\n\n"
                                         "The pre-flight checks are\n"
                                         "not here: they live in the\n"
                                         "transport groups, next to\n"
                                         "the channels they qualify.";

static const char* const HELP_ABOUT = "Version, payload count, and\n"
                                      "credits.";

/* Groups occupy 0..PifkGroupCount-1 so the row index is the group
 * id. Everything after is offset past them. */
enum {
    MainMenuIndexPayloads = PifkGroupCount,
    MainMenuIndexCaptures,
    MainMenuIndexRemoteMode,
    MainMenuIndexSettings,
    MainMenuIndexAbout,
};

static void main_menu_callback(void* context, uint32_t index) {
    PifkApp* app = context;
    scene_manager_set_scene_state(app->scene_manager, PifkSceneMainMenu, index);

    if(index < PifkGroupCount) {
        app->selected_group = (uint8_t)index;
        scene_manager_next_scene(app->scene_manager, PifkSceneChannelList);
        return;
    }

    switch(index) {
    case MainMenuIndexPayloads:
        scene_manager_next_scene(app->scene_manager, PifkScenePayloadList);
        break;
    case MainMenuIndexCaptures:
        scene_manager_next_scene(app->scene_manager, PifkSceneCaptures);
        break;
    case MainMenuIndexRemoteMode:
        scene_manager_next_scene(app->scene_manager, PifkSceneRemoteMode);
        break;
    case MainMenuIndexSettings:
        scene_manager_next_scene(app->scene_manager, PifkSceneSettings);
        break;
    case MainMenuIndexAbout:
        scene_manager_next_scene(app->scene_manager, PifkSceneAbout);
        break;
    }
}

/* pifk_menu_add_item stores label pointers rather than copying, so
 * the group rows need storage that outlives on_enter. */
static char group_labels[PifkGroupCount][24];

/* Group help, built at enter time because it names each group's channel
 * count and precondition, both of which come from the channel table. */
static char group_help[PifkGroupCount][320];

/* Scene state packs the selected row with a flag for "help is showing",
 * so Back returns to the menu rather than leaving the app. The main
 * menu's Back handler stops the app, which would be an abrupt exit from
 * a help screen. */
#define STATE_IN_HELP  0x8000u
#define STATE_ROW_MASK 0x7FFFu

static void main_menu_help_cb(void* context, uint32_t index) {
    PifkApp* app = context;
    UNUSED(index);

    uint16_t row = pifk_menu_get_selected_row(app->menu);
    const char* help = pifk_menu_get_help(app->menu, row);
    const char* label = pifk_menu_get_label(app->menu, row);
    if(!help) return;

    snprintf(app->text_buf, sizeof(app->text_buf), "%s\n\n%s", label ? label : "", help);

    uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkSceneMainMenu);
    scene_manager_set_scene_state(
        app->scene_manager, PifkSceneMainMenu, (state & STATE_ROW_MASK) | STATE_IN_HELP);

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, app->text_buf);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
}

void pifk_scene_main_menu_on_enter(void* context) {
    PifkApp* app = context;
    PifkMenu* menu = app->menu;

    static char payloads_label[32];
    snprintf(
        payloads_label, sizeof(payloads_label), "Payloads (%u)", app->payload_db->payload_count);

    pifk_menu_reset(menu);
    pifk_menu_set_callback(menu, main_menu_callback, app);
    pifk_menu_set_help_callback(menu, main_menu_help_cb);
    pifk_menu_set_header(menu, "Prompt Injection Field Kit");

    for(uint8_t g = 0; g < PifkGroupCount; g++) {
        /* The channel count tells the operator how much is behind the
         * row, which is the only thing distinguishing a group worth
         * opening from a dead end. */
        snprintf(
            group_labels[g],
            sizeof(group_labels[g]),
            "%-10s (%u)",
            pifk_group_name(g),
            (unsigned)pifk_group_channel_count(g));

        /* Name the precondition and list what is behind the row, so the
         * operator can tell from the help alone whether this is the
         * group their target allows. */
        int n = snprintf(
            group_help[g],
            sizeof(group_help[g]),
            "%u channel(s).\n\n"
            "Needs: %s.\n\n",
            (unsigned)pifk_group_channel_count(g),
            pifk_group_precondition(g));

        size_t ch_count = 0;
        const PifkChannel* chans = pifk_channels(&ch_count);
        for(size_t i = 0; i < ch_count && n > 0 && n < (int)sizeof(group_help[g]); i++) {
            if(chans[i].group != g) continue;
            n += snprintf(
                group_help[g] + n,
                sizeof(group_help[g]) - (size_t)n,
                "%s\n  %s\n",
                chans[i].label,
                chans[i].tech);
        }
        pifk_menu_add_item(menu, group_labels[g], pifk_group_icon(g), group_help[g], g);
    }

    pifk_menu_add_item(
        menu, payloads_label, &I_mg_payloads_9x9, HELP_PAYLOADS, MainMenuIndexPayloads);
    pifk_menu_add_item(menu, "Captures", &I_mg_captures_9x9, HELP_CAPTURES, MainMenuIndexCaptures);
    pifk_menu_add_item(
        menu, "Remote Mode", &I_mg_remote_9x9, HELP_REMOTE, MainMenuIndexRemoteMode);
    pifk_menu_add_item(menu, "Settings", &I_mg_settings_9x9, HELP_SETTINGS, MainMenuIndexSettings);
    pifk_menu_add_item(menu, "About", &I_mg_about_9x9, HELP_ABOUT, MainMenuIndexAbout);

    uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkSceneMainMenu);
    pifk_menu_set_selected_row(menu, (uint16_t)(state & STATE_ROW_MASK));

    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
}

bool pifk_scene_main_menu_on_event(void* context, SceneManagerEvent event) {
    PifkApp* app = context;

    if(event.type == SceneManagerEventTypeBack) {
        uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkSceneMainMenu);
        if(state & STATE_IN_HELP) {
            /* Back from help returns to the menu.  Without this the main
             * menu's Back would stop the app, which is an abrupt exit
             * from a help screen. */
            scene_manager_set_scene_state(
                app->scene_manager, PifkSceneMainMenu, state & STATE_ROW_MASK);
            view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
            return true;
        }
        /* Stop the app instead of popping back to splash */
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    return false;
}

void pifk_scene_main_menu_on_exit(void* context) {
    PifkApp* app = context;
    pifk_menu_reset(app->menu);
    text_box_reset(app->text_box);
}
