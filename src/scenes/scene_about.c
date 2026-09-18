/*
 * About scene — app info, version, credits.
 * Uses TextBox for vertical scrolling.
 */

#include "../pifk_app.h"

void pifk_scene_about_on_enter(void* context) {
    PifkApp* app = context;

    snprintf(
        app->text_buf,
        sizeof(app->text_buf),
        "Prompt Injection\n"
        "Field Kit\n"
        "Version " PIFK_VERSION "\n"
        "%u payloads\n"
        "\n"
        "github.com/Mindgard/\n"
        "flipperzero-prompt-\n"
        "injection-field-kit\n"
        "\n"
        "Apache-2.0\n"
        "(c) 2026 Mindgard Ltd\n"
        "\n"
        "Flipper Zero is a\n"
        "trademark of Flipper\n"
        "Devices Inc. This app\n"
        "is unaffiliated.\n"
        "\n"
        "Authorised testing\n"
        "only.",
        app->payload_db->payload_count);

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, app->text_buf);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
}

bool pifk_scene_about_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void pifk_scene_about_on_exit(void* context) {
    PifkApp* app = context;
    text_box_reset(app->text_box);
}
