/*
 * Remote mode scene — serial bridge for control from Prompt Injection
 * Studio
 * via `hw flipper remote`.
 *
 * Entering starts the bridge thread and switches USB to dual-CDC;
 * leaving stops it and restores single-CDC.  The bridge only exists
 * while this scene is on screen, so Back is the way to release the
 * port.
 *
 * The bridge state is owned by the bridge thread, so this screen polls
 * it on a timer rather than being pushed updates.  The timer callback
 * posts a custom event and the redraw happens on the main thread, which
 * is the only thread allowed to touch the TextBox.
 *
 * The text is only re-set when something visible changed.  TextBox
 * keeps the pointer we hand it rather than copying, so rewriting the
 * buffer on every tick would race its draw callback for no reason.
 */

#include "../pifk_app.h"
#include "../serial/bridge_protocol.h"

#define REMOTE_EVENT_REFRESH 0
#define REMOTE_REFRESH_MS    250

static const char* bridge_state_text(PifkBridgeState state) {
    switch(state) {
    case PifkBridgeListening:
        return "Listening";
    case PifkBridgeConnected:
        return "Connected";
    case PifkBridgeExecuting:
        return "Executing";
    case PifkBridgeError:
        return "Error";
    default:
        return "Starting...";
    }
}

/* Snapshot of what the screen currently shows, so a tick that changed
 * nothing does no work.  Scene-local: reset by on_enter. */
static PifkBridgeState last_state;
static uint16_t last_payloads;
static bool snapshot_valid;

/* Rebuild the body only if the state or record counts moved.  Returns
 * true when the buffer changed and the TextBox needs re-pointing. */
static bool remote_mode_render(PifkApp* app) {
    PifkBridgeState state = app->bridge_state;
    uint16_t payloads = app->payload_db->payload_count;

    if(snapshot_valid && state == last_state && payloads == last_payloads && true) {
        return false;
    }

    last_state = state;
    last_payloads = payloads;
    snapshot_valid = true;

    snprintf(
        app->text_buf,
        sizeof(app->text_buf),
        "Remote Mode: %s\n"
        "USB CDC @ 115200\n\n"
        "In Prompt Injection\n"
        "Studio run:\n"
        "  hw flipper remote\n\n"
        "%u payloads\n"
        "Back to cancel.",
        bridge_state_text(state),
        payloads);

    return true;
}

static void remote_mode_timer_cb(void* context) {
    PifkApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, REMOTE_EVENT_REFRESH);
}

void pifk_scene_remote_mode_on_enter(void* context) {
    PifkApp* app = context;

    /* Draw once before starting the bridge so the screen is populated
     * during USB re-enumeration, which takes about 100ms. */
    snapshot_valid = false;
    remote_mode_render(app);
    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, app->text_buf);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);

    bridge_start(app);

    app->remote_refresh_timer = furi_timer_alloc(remote_mode_timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->remote_refresh_timer, REMOTE_REFRESH_MS);
}

bool pifk_scene_remote_mode_on_event(void* context, SceneManagerEvent event) {
    PifkApp* app = context;

    if(event.type == SceneManagerEventTypeCustom && event.event == REMOTE_EVENT_REFRESH) {
        if(remote_mode_render(app)) {
            /* Re-point the TextBox at the rebuilt buffer.  text_box_reset()
             * would drop the scroll position, so only the text is set. */
            text_box_set_text(app->text_box, app->text_buf);
        }
        return true;
    }
    return false;
}

void pifk_scene_remote_mode_on_exit(void* context) {
    PifkApp* app = context;

    /* Stop polling before the bridge goes away. */
    if(app->remote_refresh_timer) {
        furi_timer_stop(app->remote_refresh_timer);
        furi_timer_free(app->remote_refresh_timer);
        app->remote_refresh_timer = NULL;
    }

    bridge_stop(app);
    text_box_reset(app->text_box);
}
