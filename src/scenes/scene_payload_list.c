/*
 * Payload list scene — the unfiltered payload library.
 *
 * Reading the whole set to learn what prompt injection looks like is a
 * legitimate use of the kit, and it is payload-first by nature, so this
 * list is not filtered by channel: selecting an entry opens its detail
 * view, which offers the channels that accept it.
 *
 * Starred payloads sort to the top and long-press OK stars one — both
 * from the shared builder in payload_picker.c, which is why the
 * favorites scene went away.
 */

#include "../pifk_app.h"
#include "payload_picker.h"

static void payload_list_callback(void* context, uint32_t index) {
    PifkApp* app = context;
    if(index < app->payload_db->payload_count) {
        app->selected_payload_index = (uint16_t)index;
        scene_manager_set_scene_state(app->scene_manager, PifkScenePayloadList, index);
        scene_manager_next_scene(app->scene_manager, PifkScenePayloadView);
    }
}

void pifk_scene_payload_list_on_enter(void* context) {
    PifkApp* app = context;

    pifk_menu_reset(app->menu);

    /* Header must outlive this call: the menu keeps the
     * pointer rather than copying. */
    static char header[48];
    pifk_payload_picker_header(app, NULL, header, sizeof(header));
    pifk_menu_set_header(app->menu, header);

    pifk_payload_picker_build(app, NULL, payload_list_callback);

    uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkScenePayloadList);
    pifk_menu_set_selected_row(app->menu, (uint16_t)state);

    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
}

bool pifk_scene_payload_list_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void pifk_scene_payload_list_on_exit(void* context) {
    PifkApp* app = context;
    pifk_menu_reset(app->menu);
}
