/*
 * Channel list scene — the channels within one transport group.
 *
 * ┌──────────────────────────┐
 * │ Wires - header + GND     │
 * │ > Send + capture         │
 * │   Send it                │
 * │   Write to a chip        │
 * │   ─ check the wiring ─   │
 * │   Scan the I2C bus       │
 * │   Loopback test          │
 * └──────────────────────────┘
 *
 * The header carries the group's physical precondition rather than a
 * row, because it answers "am I even in the right group?" and a 128x64
 * screen cannot spare a line to say it twice.
 *
 * Diagnostics sit below the delivery channels behind a separator. Both
 * answer the same question — is the wire good before I blame the
 * payload? — and they were previously split between Quick Deploy (I2C
 * scan) and Settings (GPIO loopback), which put the two halves of one
 * diagnosis two menus apart. Neither takes a payload, which is why they
 * are grouped away from the channels that do.
 */

#include "../pifk_app.h"
#include "../channel/channel.h"
#include "pifk_icons.h"
#include "../execute/gpio_exec.h"
#include "../execute/i2c_exec.h"
#include "../execute/badusb_exec.h"
#include <string.h>

/* Diagnostic row ids, offset past every channel id so a row index can
 * never be mistaken for a channel. */
#define DIAG_I2C_SCAN      0x100
#define DIAG_GPIO_LOOPBACK 0x101
#define DIAG_USB_HID_PROBE 0x102

/* Scene state packs the selected row with a flag for "a diagnostic's
 * TextBox is showing", so Back returns to this menu rather than leaving
 * the group mid-diagnosis. The same approach scene_payload_view.c uses
 * for its text view; there is no API to ask the dispatcher which view
 * is current. */
#define STATE_IN_TEXT_BOX 0x8000u
#define STATE_ROW_MASK    0x7FFFu

/* Labels outlive on_enter because the menu stores the pointer. */
static char rows[PifkChannelIdCount][40];

/* ── Diagnostics ─────────────────────────────────────────────── */

/* Probe the I2C bus and report what answered.
 *
 * Diagnostic rather than delivery: an unknown bus is the normal case,
 * and writing to a guessed address can be destructive — a PMIC's
 * control registers are as writable as an EEPROM's. */
static void channel_list_i2c_scan(PifkApp* app) {
    PifkI2cScan scan;
    notification_message(app->notifications, &sequence_blink_start_cyan);
    bool ok = pifk_i2c_scan(app, &scan);
    notification_message(app->notifications, &sequence_blink_stop);

    int32_t scl = -1, sda = -1;
    pifk_i2c_pins(&scl, &sda);

    if(!ok) {
        const char* err = pifk_i2c_last_error();
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "%s\n\n"
            "SCL is pin %ld, SDA is\n"
            "pin %ld. Pull-ups are\n"
            "the target board's job;\n"
            "a bus without them\n"
            "reads as empty.",
            err ? err : "Scan failed.",
            (long)scl,
            (long)sda);
    } else {
        int n = snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "%u device(s) on pins\n"
            "%ld (SCL) / %ld (SDA):\n\n",
            (unsigned)scan.total,
            (long)scl,
            (long)sda);
        for(uint8_t i = 0; i < scan.count && n > 0 && n < (int)sizeof(app->text_buf); i++) {
            /* Name the two families an operator is most likely to meet,
             * so the number means something. These are conventions, not
             * identification: 0x68 could be an RTC or an IMU. */
            const char* guess = "";
            if(scan.addr[i] >= 0x50 && scan.addr[i] <= 0x57) {
                guess = "  EEPROM?";
            } else if(scan.addr[i] == 0x3C || scan.addr[i] == 0x3D) {
                guess = "  display?";
            }
            n += snprintf(
                app->text_buf + n,
                sizeof(app->text_buf) - (size_t)n,
                "  0x%02X%s\n",
                scan.addr[i],
                guess);
        }
        if(scan.total > scan.count && n > 0 && n < (int)sizeof(app->text_buf)) {
            snprintf(
                app->text_buf + n,
                sizeof(app->text_buf) - (size_t)n,
                "\n(+%u more)",
                (unsigned)(scan.total - scan.count));
        }
    }

    /* TextBox rather than a dialog: a full bus needs scrolling. */
    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, app->text_buf);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
    notification_message(app->notifications, ok ? &sequence_success : &sequence_error);
}

/* Bridge TX to RX with a jumper and this proves the transport works
 * before the target's hardware is in question — the only channel here
 * that can, which is what turns "it didn't work" into a diagnosis. */
static void channel_list_gpio_loopback(PifkApp* app) {
    size_t sent = 0, received = 0;

    notification_message(app->notifications, &sequence_blink_start_cyan);
    bool ok = pifk_gpio_loopback_test(app, &sent, &received);
    notification_message(app->notifications, &sequence_blink_stop);

    int32_t tx = -1, rx = -1;
    pifk_gpio_pins(app, &tx, &rx);

    if(ok) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "PASS - %u bytes went\n"
            "out pin %ld and came\n"
            "back in pin %ld at %lu\n"
            "baud.\n\n"
            "The UART works. If a\n"
            "target still hears\n"
            "nothing, check GND and\n"
            "the target's baud.",
            (unsigned)sent,
            (long)tx,
            (long)rx,
            (unsigned long)app->gpio_baud);
    } else if(received == 0) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "No data came back.\n\n"
            "Bridge pin %ld (TX) to\n"
            "pin %ld (RX) with a\n"
            "jumper and retry. If it\n"
            "still fails the port is\n"
            "busy - try LPUART in\n"
            "Settings > GPIO.",
            (long)tx,
            (long)rx);
    } else {
        /* Partial return is the interesting failure: the wiring is
         * right and the baud rate is not. */
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "Got %u of %u bytes back\n"
            "at %lu baud.\n\n"
            "The jumper is on, so\n"
            "this is a baud or\n"
            "timing problem rather\n"
            "than wiring.",
            (unsigned)received,
            (unsigned)sent,
            (unsigned long)app->gpio_baud);
    }

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, app->text_buf);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
    notification_message(app->notifications, ok ? &sequence_success : &sequence_error);
}

/* USB HID host probe.
 *
 * Answers one question: will the attached USB host accept a generic HID
 * keyboard? Motivated by label printers whose USB-A host port is
 * documented as taking "HID class" peripherals like barcode scanners.
 * If that port refuses us, BadUSB cannot work against it, and it is
 * cheaper to learn that here than from a payload that silently went
 * nowhere.
 *
 * Types nothing. Switches to HID, waits for enumeration, restores. */
#define HID_PROBE_TIMEOUT_MS 3000

static void channel_list_hid_probe(PifkApp* app) {
    uint32_t waited = 0;

    notification_message(app->notifications, &sequence_blink_start_cyan);
    bool ok = pifk_badusb_probe_host(app, HID_PROBE_TIMEOUT_MS, &waited);
    notification_message(app->notifications, &sequence_blink_stop);

    if(ok) {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "PASS - the host\n"
            "enumerated us as a\n"
            "keyboard in %lums.\n\n"
            "BadUSB will work\n"
            "against this port.\n"
            "Nothing was typed.",
            (unsigned long)waited);
    } else {
        snprintf(
            app->text_buf,
            sizeof(app->text_buf),
            "No host responded in\n"
            "%dms.\n\n"
            "Either nothing is\n"
            "plugged in, or the port\n"
            "refuses HID devices.\n"
            "BadUSB will not work\n"
            "against it.",
            HID_PROBE_TIMEOUT_MS);
    }

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, app->text_buf);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
    notification_message(app->notifications, ok ? &sequence_success : &sequence_error);
}

/* Help for the diagnostic rows.  The channels carry their own help in
 * the channel table; these three do not live there because they deliver
 * nothing. */
static const char* const HELP_I2C_SCAN = "Probes every address on the\n"
                                         "I2C bus and lists what\n"
                                         "answers.\n\n"
                                         "Do this before writing.\n"
                                         "Writing to a guessed\n"
                                         "address is destructive in a\n"
                                         "way UART is not: an\n"
                                         "unexpected device might be\n"
                                         "a PMIC, and payload text in\n"
                                         "its control registers is a\n"
                                         "bricked board.\n\n"
                                         "0x50-0x57 is usually an\n"
                                         "EEPROM, 0x3C/0x3D usually a\n"
                                         "display, but these are\n"
                                         "conventions and not\n"
                                         "identification.\n\n"
                                         "Pull-ups are the target\n"
                                         "board's job. A bus without\n"
                                         "them reads as completely\n"
                                         "empty, which looks exactly\n"
                                         "like nothing being wired.";

static const char* const HELP_GPIO_LOOPBACK = "Bridge the TX and RX pins\n"
                                              "with a jumper, then run\n"
                                              "this. It sends a known\n"
                                              "string, reads it back, and\n"
                                              "compares.\n\n"
                                              "The only transport here\n"
                                              "that can be proved before\n"
                                              "your target is involved,\n"
                                              "which is what turns 'it\n"
                                              "didn't work' into a\n"
                                              "diagnosis.\n\n"
                                              "PASS: the UART works. If a\n"
                                              "target still hears nothing,\n"
                                              "check GND and its baud.\n\n"
                                              "No data: the jumper is not\n"
                                              "making contact, or the port\n"
                                              "is busy - try LPUART.\n\n"
                                              "Partial: wiring is right\n"
                                              "and the baud rate is wrong.";

static const char* const HELP_HID_PROBE = "Asks whether the attached\n"
                                          "USB host will accept a\n"
                                          "generic HID keyboard.\n"
                                          "Types nothing.\n\n"
                                          "Worth running before you\n"
                                          "rely on BadUSB against\n"
                                          "anything that is not a\n"
                                          "normal computer. Printers,\n"
                                          "kiosks and industrial\n"
                                          "panels are much fussier\n"
                                          "than a desktop.\n\n"
                                          "PASS means the link came\n"
                                          "up, not that the host\n"
                                          "accepted us. A Brother\n"
                                          "QL-820NWB passed in 150ms\n"
                                          "and then displayed 'USB\n"
                                          "device non-compliant' - our\n"
                                          "HID interface is a\n"
                                          "composite device and some\n"
                                          "embedded hosts want a plain\n"
                                          "boot keyboard.\n\n"
                                          "Read PASS as 'worth\n"
                                          "trying', and watch the\n"
                                          "target's own display.";

/* ── Navigation ──────────────────────────────────────────────── */

/* Right on a row shows its help.  The row's own help pointer is read
 * back from the widget rather than looked up again here, so the two
 * cannot disagree about which row is selected. */
static void channel_list_help_cb(void* context, uint32_t index) {
    PifkApp* app = context;
    UNUSED(index);

    uint16_t row = pifk_menu_get_selected_row(app->menu);
    const char* help = pifk_menu_get_help(app->menu, row);
    const char* label = pifk_menu_get_label(app->menu, row);
    if(!help) return;

    snprintf(app->text_buf, sizeof(app->text_buf), "%s\n\n%s", label ? label : "", help);

    uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkSceneChannelList);
    scene_manager_set_scene_state(
        app->scene_manager, PifkSceneChannelList, (state & STATE_ROW_MASK) | STATE_IN_TEXT_BOX);

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, app->text_buf);
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewTextBox);
}

static void channel_list_callback(void* context, uint32_t index) {
    PifkApp* app = context;
    uint32_t row = scene_manager_get_scene_state(app->scene_manager, PifkSceneChannelList) &
                   STATE_ROW_MASK;

    if(index >= DIAG_I2C_SCAN) {
        /* Remember that a TextBox is up so Back comes here, not out. */
        scene_manager_set_scene_state(
            app->scene_manager, PifkSceneChannelList, row | STATE_IN_TEXT_BOX);
        if(index == DIAG_I2C_SCAN) {
            channel_list_i2c_scan(app);
        } else if(index == DIAG_GPIO_LOOPBACK) {
            channel_list_gpio_loopback(app);
        } else {
            channel_list_hid_probe(app);
        }
        return;
    }

    scene_manager_set_scene_state(app->scene_manager, PifkSceneChannelList, index);
    app->selected_channel = (uint8_t)index;
    scene_manager_next_scene(app->scene_manager, PifkScenePayloadPick);
}

void pifk_scene_channel_list_on_enter(void* context) {
    PifkApp* app = context;
    PifkMenu* menu = app->menu;
    PifkChannelGroup group = app->selected_group;

    pifk_menu_reset(menu);
    pifk_menu_set_callback(menu, channel_list_callback, app);
    pifk_menu_set_help_callback(menu, channel_list_help_cb);

    /* Group name plus what the operator has to do for it to work. */
    static char header[40];
    snprintf(
        header, sizeof(header), "%s - %s", pifk_group_name(group), pifk_group_precondition(group));
    pifk_menu_set_header(menu, header);

    size_t count = 0;
    const PifkChannel* channels = pifk_channels(&count);

    for(size_t i = 0; i < count; i++) {
        const PifkChannel* ch = &channels[i];
        if(ch->group != group) continue;

        /* Both the operator's verb and the technology: the first is what
         * you choose by, the second is what goes in the report. */
        snprintf(rows[ch->id], sizeof(rows[ch->id]), "%s  (%s)", ch->label, ch->tech);
        pifk_menu_add_item(menu, rows[ch->id], ch->icon, ch->help, ch->id);
    }

    /* Pre-flight checks, below the channels they qualify.  Each answers
     * "is the transport good before I blame the payload?" — the question
     * that separates a failed injection from a failed wire.  Only USB and
     * Wires have one, because those are the groups whose transport can be
     * silently broken; a QR code either renders or does not. */
    if(group == PifkGroupWires) {
        pifk_menu_add_item(
            menu, "Scan the I2C bus", &I_mg_diagnostic_9x9, HELP_I2C_SCAN, DIAG_I2C_SCAN);
        pifk_menu_add_item(
            menu,
            "UART loopback test",
            &I_mg_diagnostic_9x9,
            HELP_GPIO_LOOPBACK,
            DIAG_GPIO_LOOPBACK);
    } else if(group == PifkGroupUsb) {
        pifk_menu_add_item(
            menu, "HID host probe", &I_mg_diagnostic_9x9, HELP_HID_PROBE, DIAG_USB_HID_PROBE);
    }

    uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkSceneChannelList);
    pifk_menu_set_selected_row(menu, (uint16_t)(state & STATE_ROW_MASK));

    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
}

bool pifk_scene_channel_list_on_event(void* context, SceneManagerEvent event) {
    PifkApp* app = context;

    /* Back out of a diagnostic's TextBox returns to this menu rather
     * than leaving the group, since the operator is mid-diagnosis. */
    if(event.type == SceneManagerEventTypeBack) {
        uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkSceneChannelList);
        if(state & STATE_IN_TEXT_BOX) {
            scene_manager_set_scene_state(
                app->scene_manager, PifkSceneChannelList, state & STATE_ROW_MASK);
            view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewMenu);
            return true;
        }
    }
    return false;
}

void pifk_scene_channel_list_on_exit(void* context) {
    PifkApp* app = context;
    /* Clear the TextBox flag: a scene re-entered later should open on
     * its menu, not believe a diagnostic is still showing. */
    uint32_t state = scene_manager_get_scene_state(app->scene_manager, PifkSceneChannelList);
    scene_manager_set_scene_state(
        app->scene_manager, PifkSceneChannelList, state & STATE_ROW_MASK);

    pifk_menu_reset(app->menu);
    text_box_reset(app->text_box);
}
