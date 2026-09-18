/*
 * Settings scene — configurable app parameters.
 *
 * - BadUSB start delay (ms)
 * - GPIO / UART baud, port, line ending, pacing, listen window
 * - I2C address
 * - Read-only pin reminders for both, resolved from the firmware
 *
 * Every change writes settings.json immediately, so there is no save
 * step and no unsaved state to lose.
 *
 * What is *not* here: the UART loopback test and the USB HID host probe
 * moved to their transport groups (Main Menu > Wires and > USB), next to
 * the channels they qualify. Settings holds parameters; a check you run
 * against a target belongs beside the thing it tests.
 */

#include "../pifk_app.h"
#include "../channel/channel.h"
#include "../payload/payload_db.h"
#include "../execute/gpio_exec.h"
#include "../execute/i2c_exec.h"
#include "../execute/badusb_exec.h"

/* There is deliberately no default-channel setting.
 *
 * Its predecessor, default_protocol, was written by this screen and read
 * by nothing: every bridge EXEC command names its own channel, and
 * navigation is transport-first, so the operator picks a channel by
 * walking into it. A picker that changes nothing is worse than no
 * picker. */

static void badusb_delay_change_cb(VariableItem* item) {
    PifkApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);

    static const uint32_t delays[] = {
        100,
        250,
        500,
        750,
        1000,
        1500,
        2000,
        3000,
        5000,
    };
    static const char* delay_labels[] = {
        "100ms",
        "250ms",
        "500ms",
        "750ms",
        "1000ms",
        "1500ms",
        "2000ms",
        "3000ms",
        "5000ms",
    };
#define DELAY_COUNT (sizeof(delays) / sizeof(delays[0]))

    if(idx < DELAY_COUNT) {
        app->badusb_delay_ms = delays[idx];
        variable_item_set_current_value_text(item, delay_labels[idx]);
        settings_save(app, PIFK_SETTINGS_FILE);
    }
}

/* ── GPIO / serial settings ──────────────────────────────────── */

static const uint32_t gpio_bauds[] = {
    9600,
    19200,
    38400,
    57600,
    115200,
    230400,
    460800,
    921600,
};
static const char* gpio_baud_labels[] = {
    "9600",
    "19200",
    "38400",
    "57600",
    "115200",
    "230400",
    "460800",
    "921600",
};
#define GPIO_BAUD_COUNT (sizeof(gpio_bauds) / sizeof(gpio_bauds[0]))

static const char* gpio_port_labels[] = {"USART", "LPUART"};
static const char* gpio_ending_labels[] = {"none", "LF", "CRLF"};

static void gpio_baud_change_cb(VariableItem* item) {
    PifkApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx < GPIO_BAUD_COUNT) {
        app->gpio_baud = gpio_bauds[idx];
        variable_item_set_current_value_text(item, gpio_baud_labels[idx]);
        settings_save(app, PIFK_SETTINGS_FILE);
    }
}

static void gpio_port_change_cb(VariableItem* item) {
    PifkApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx < 2) {
        app->gpio_serial_id = idx;
        variable_item_set_current_value_text(item, gpio_port_labels[idx]);
        settings_save(app, PIFK_SETTINGS_FILE);
    }
}

static void gpio_ending_change_cb(VariableItem* item) {
    PifkApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx < PifkGpioLineEndingCount) {
        app->gpio_line_ending = idx;
        variable_item_set_current_value_text(item, gpio_ending_labels[idx]);
        settings_save(app, PIFK_SETTINGS_FILE);
    }
}

/* ── I2C settings ────────────────────────────────────────────── *
 *
 * A picker rather than a 0x08-0x77 range: 112 values is unusable when
 * each one needs a separate press. These are the addresses an operator
 * actually meets — the 24Cxx EEPROM bank and the two common SSD1306
 * display addresses. An arbitrary address can be set as i2c_address in
 * settings.json, which the loader range-checks.
 *
 * Scan the bus first; the write path refuses an address that does not
 * ACK, so a wrong choice here fails safe rather than writing blind. */
static const uint8_t i2c_addresses[] = {0x3C, 0x3D, 0x50, 0x51, 0x52, 0x53, 0x54, 0x57};
static const char* i2c_address_labels[] = {
    "0x3C",
    "0x3D",
    "0x50",
    "0x51",
    "0x52",
    "0x53",
    "0x54",
    "0x57",
};
#define I2C_ADDRESS_COUNT (sizeof(i2c_addresses) / sizeof(i2c_addresses[0]))

static void i2c_address_change_cb(VariableItem* item) {
    PifkApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx < I2C_ADDRESS_COUNT) {
        app->i2c_address = i2c_addresses[idx];
        variable_item_set_current_value_text(item, i2c_address_labels[idx]);
        settings_save(app, PIFK_SETTINGS_FILE);
    }
}

/* 0 means burst-send; anything else paces and makes Stop effective. */
static const uint32_t gpio_byte_delays[] = {0, 1, 2, 5, 10, 20, 50};
static const char* gpio_byte_delay_labels[] = {
    "off",
    "1ms",
    "2ms",
    "5ms",
    "10ms",
    "20ms",
    "50ms",
};
#define GPIO_BYTE_DELAY_COUNT (sizeof(gpio_byte_delays) / sizeof(gpio_byte_delays[0]))

static void gpio_byte_delay_change_cb(VariableItem* item) {
    PifkApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx < GPIO_BYTE_DELAY_COUNT) {
        app->gpio_byte_delay_ms = gpio_byte_delays[idx];
        variable_item_set_current_value_text(item, gpio_byte_delay_labels[idx]);
        settings_save(app, PIFK_SETTINGS_FILE);
    }
}

static const uint32_t gpio_listens[] = {200, 500, 1000, 2000, 5000, 10000};
static const char* gpio_listen_labels[] = {"200ms", "500ms", "1s", "2s", "5s", "10s"};
#define GPIO_LISTEN_COUNT (sizeof(gpio_listens) / sizeof(gpio_listens[0]))

static void gpio_listen_change_cb(VariableItem* item) {
    PifkApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx < GPIO_LISTEN_COUNT) {
        app->gpio_listen_ms = gpio_listens[idx];
        variable_item_set_current_value_text(item, gpio_listen_labels[idx]);
        settings_save(app, PIFK_SETTINGS_FILE);
    }
}

/* Row counter, kept while the list is built.
 *
 * The pre-flight diagnostics that used to live here — the UART loopback
 * test and the USB HID host probe — moved to their transport groups
 * (Main Menu > Wires and > USB). Settings holds parameters; a check you
 * run against a target belongs next to the channel it checks. */
static uint8_t action_row_next;

/* Find the index of a value in an array of uint32_t. */
static uint8_t find_delay_index(const uint32_t* arr, size_t count, uint32_t value) {
    for(size_t i = 0; i < count; i++) {
        if(arr[i] >= value) return (uint8_t)i;
    }
    return 0;
}

void pifk_scene_settings_on_enter(void* context) {
    PifkApp* app = context;
    VariableItemList* list = app->var_item_list;

    variable_item_list_reset(list);

    action_row_next = 0;

    /* BadUSB start delay */
    static const uint32_t badusb_delays[] = {
        100,
        250,
        500,
        750,
        1000,
        1500,
        2000,
        3000,
        5000,
    };
    static const char* badusb_delay_labels[] = {
        "100ms",
        "250ms",
        "500ms",
        "750ms",
        "1000ms",
        "1500ms",
        "2000ms",
        "3000ms",
        "5000ms",
    };

    VariableItem* item;
    item = variable_item_list_add(list, "BadUSB Delay", 9, badusb_delay_change_cb, app);
    action_row_next++;
    uint8_t idx = find_delay_index(badusb_delays, 9, app->badusb_delay_ms);
    variable_item_set_current_value_index(item, idx);
    variable_item_set_current_value_text(item, badusb_delay_labels[idx]);

    /* Conversation inter-turn delay */
    /* ── GPIO / serial ───────────────────────────────────────── */

    item = variable_item_list_add(list, "GPIO Baud", GPIO_BAUD_COUNT, gpio_baud_change_cb, app);
    action_row_next++;
    uint8_t gpio_idx = 4; /* 115200 */
    for(size_t i = 0; i < GPIO_BAUD_COUNT; i++) {
        if(gpio_bauds[i] == app->gpio_baud) {
            gpio_idx = (uint8_t)i;
            break;
        }
    }
    variable_item_set_current_value_index(item, gpio_idx);
    variable_item_set_current_value_text(item, gpio_baud_labels[gpio_idx]);

    item = variable_item_list_add(list, "GPIO Port", 2, gpio_port_change_cb, app);
    action_row_next++;
    uint8_t port_idx = (app->gpio_serial_id < 2) ? app->gpio_serial_id : 0;
    variable_item_set_current_value_index(item, port_idx);
    variable_item_set_current_value_text(item, gpio_port_labels[port_idx]);

    item = variable_item_list_add(
        list, "GPIO Newline", PifkGpioLineEndingCount, gpio_ending_change_cb, app);
    action_row_next++;
    uint8_t end_idx = (app->gpio_line_ending < PifkGpioLineEndingCount) ?
                          app->gpio_line_ending :
                          (uint8_t)PifkGpioLineEndingLf;
    variable_item_set_current_value_index(item, end_idx);
    variable_item_set_current_value_text(item, gpio_ending_labels[end_idx]);

    item = variable_item_list_add(
        list, "GPIO Pacing", GPIO_BYTE_DELAY_COUNT, gpio_byte_delay_change_cb, app);
    action_row_next++;
    uint8_t bd_idx = 0;
    for(size_t i = 0; i < GPIO_BYTE_DELAY_COUNT; i++) {
        if(gpio_byte_delays[i] == app->gpio_byte_delay_ms) {
            bd_idx = (uint8_t)i;
            break;
        }
    }
    variable_item_set_current_value_index(item, bd_idx);
    variable_item_set_current_value_text(item, gpio_byte_delay_labels[bd_idx]);

    item =
        variable_item_list_add(list, "GPIO Listen", GPIO_LISTEN_COUNT, gpio_listen_change_cb, app);
    action_row_next++;
    uint8_t ls_idx = 2; /* 1s */
    for(size_t i = 0; i < GPIO_LISTEN_COUNT; i++) {
        if(gpio_listens[i] == app->gpio_listen_ms) {
            ls_idx = (uint8_t)i;
            break;
        }
    }
    variable_item_set_current_value_index(item, ls_idx);
    variable_item_set_current_value_text(item, gpio_listen_labels[ls_idx]);

    /* Read-only pin reminder. "p13>14" reads as "bridge pin 13 to pin
     * 14", which is the whole instruction for the loopback test under
     * Wires. The value column is ~10 chars so there is no room for
     * prose, and the numbers come from the firmware rather than a
     * printed diagram, so they cannot drift. */
    int32_t tx = -1, rx = -1;
    pifk_gpio_pins(app, &tx, &rx);
    static char gpio_pin_hint[24];
    snprintf(gpio_pin_hint, sizeof(gpio_pin_hint), "p%ld>%ld", (long)tx, (long)rx);
    item = variable_item_list_add(list, "GPIO Pins TX>RX", 1, NULL, app);
    action_row_next++;
    variable_item_set_current_value_text(item, gpio_pin_hint);

    /* ── I2C ─────────────────────────────────────────────────── */

    item =
        variable_item_list_add(list, "I2C Address", I2C_ADDRESS_COUNT, i2c_address_change_cb, app);
    action_row_next++;
    uint8_t addr_idx = 2; /* 0x50 */
    for(size_t i = 0; i < I2C_ADDRESS_COUNT; i++) {
        if(i2c_addresses[i] == app->i2c_address) {
            addr_idx = (uint8_t)i;
            break;
        }
    }
    variable_item_set_current_value_index(item, addr_idx);
    variable_item_set_current_value_text(item, i2c_address_labels[addr_idx]);

    /* Read-only pin reminder, same reasoning as the loopback hint: the
     * numbers come from the firmware rather than a printed diagram. */
    int32_t scl = -1, sda = -1;
    pifk_i2c_pins(&scl, &sda);
    static char i2c_pin_hint[24];
    snprintf(i2c_pin_hint, sizeof(i2c_pin_hint), "p%ld/%ld", (long)scl, (long)sda);
    item = variable_item_list_add(list, "I2C Pins SCL/SDA", 1, NULL, app);
    action_row_next++;
    variable_item_set_current_value_text(item, i2c_pin_hint);

    view_dispatcher_switch_to_view(app->view_dispatcher, PifkViewVariableItemList);
}

bool pifk_scene_settings_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void pifk_scene_settings_on_exit(void* context) {
    PifkApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
