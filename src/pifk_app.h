#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/text_box.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_input.h>
#include <gui/modules/popup.h>
#include <gui/view.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>
#include "gui/pifk_menu.h"

/* ── Version ─────────────────────────────────────────────────── *
 *
 * Keep in sync with fap_version in application.fam. */

#define PIFK_VERSION "1.1"

/* ── App data paths ──────────────────────────────────────────── */

#define PIFK_APP_DIR        APP_DATA_PATH("")
#define PIFK_PAYLOADS_FILE  APP_DATA_PATH("payloads.json")
#define PIFK_FAVORITES_FILE APP_DATA_PATH("favorites.json")
#define PIFK_SETTINGS_FILE  APP_DATA_PATH("settings.json")

/* Captured target replies, one JSON object per line.  Written by the
 * device and read by the host, unlike the other files here — see
 * payload/capture_log.h.  The .1 generation is the single rotated
 * predecessor, overwritten each time the log fills. */
#define PIFK_CAPTURES_FILE     APP_DATA_PATH("captures.jsonl")
#define PIFK_CAPTURES_OLD_FILE APP_DATA_PATH("captures.jsonl.1")

/* ── Limits ──────────────────────────────────────────────────── */

/* PayloadDb is a fixed inline array, so this is ~216 bytes of heap per
 * slot whether or not a payload occupies it — 12 KB at 56, on a device
 * reporting ~37 KB free.  Raised from 48 when the builtin set reached 45
 * and left only three slots for the user's own payloads.json; every
 * further increase costs 1.7 KB per 8 slots, so raise it deliberately
 * rather than for headroom's sake. */
#define PIFK_MAX_PAYLOADS     56
#define PIFK_MAX_NAME_LEN     48
#define PIFK_MAX_TEXT_LEN     512
#define PIFK_MAX_DESC_LEN     128
#define PIFK_MAX_CATEGORY_LEN 32
#define PIFK_MAX_FAVORITES    8
#define PIFK_SERIAL_BUF_SIZE  256

/* Upper bound on persisted/remote-supplied delays.  Without this a
 * hostile or corrupt settings.json can request a multi-day
 * furi_delay_ms() that the user cannot interrupt. */
#define PIFK_MAX_DELAY_MS 60000

/* Largest JSON file we will read into heap in one piece.
 *
 * This is a heap budget, not a format limit.  It was 63 KB — chosen only
 * to stay under the uint16_t ceiling on storage_file_read() lengths —
 * which is larger than the device's entire free heap, so the check could
 * never reject a file before malloc() failed.  8 KB is a cap the ~37 KB
 * free heap can absorb alongside the ~12 KB payload database and the GUI.
 *
 * This bounds every whole-file read: payloads.json, favorites.json and
 * settings.json.  8 KB is ample for the latter two, and for a payloads.json
 * holding only user entries now that the builtins are no longer exported
 * into it — a payloads.json above this is rejected rather than risking the
 * allocation, so the app starts with the builtins alone. */
#define PIFK_MAX_FILE_SIZE (8 * 1024)

/* Byte-mode capacity of the largest QR version the encoder supports
 * (V6, ECC-L).  Kept here so the UI and the serial bridge report the
 * same limit the encoder actually enforces. */
#define PIFK_QR_MAX_BYTES 134

/* ── Payload model (mirrors Python Payload dataclass) ────────── */

typedef struct {
    char name[PIFK_MAX_NAME_LEN];
    const char* text; /* points to flash literal (builtin) or heap (user) */
    char category[PIFK_MAX_CATEGORY_LEN];
    char description[PIFK_MAX_DESC_LEN];
    bool is_builtin;
    bool is_favorite;
    bool text_owned; /* true if text was malloc'd and needs free */
} PifkPayload;

/* ── Payload database ────────────────────────────────────────── */

typedef struct {
    PifkPayload payloads[PIFK_MAX_PAYLOADS];
    uint16_t payload_count;
} PayloadDb;

/* ── Scene IDs ───────────────────────────────────────────────── */

/* Scene ids are not persisted, so this enum is free to change.
 *
 * PifkSceneFavorites was removed: starred payloads sort to the top
 * of every payload list, which is where they are useful, so a separate
 * filtered screen earned neither its top-level row nor its scene.
 *
 * PifkSceneQuickDeploy was replaced by ChannelList (channels in a
 * group) plus PayloadPick (payloads a channel can carry). */
typedef enum {
    PifkSceneMainMenu,
    PifkSceneChannelList,
    PifkScenePayloadPick,
    PifkScenePayloadList,
    PifkScenePayloadView,
    PifkSceneRemoteMode,
    PifkSceneSettings,
    PifkSceneAbout,
    PifkSceneCaptures,
    PifkSceneCount,
} PifkScene;

/* ── View IDs ────────────────────────────────────────────────── */

typedef enum {
    PifkViewTextBox,
    PifkViewVariableItemList,
    PifkViewTextInput,
    PifkViewPopup,
    PifkViewQrCode,
    PifkViewMenu,
} PifkView;

/* ── Bridge state ────────────────────────────────────────────── */

typedef enum {
    PifkBridgeIdle,
    PifkBridgeListening,
    PifkBridgeConnected,
    PifkBridgeExecuting,
    PifkBridgeError,
} PifkBridgeState;

/* ── Deferred bridge requests ────────────────────────────────── *
 *
 * The bridge runs on its own thread.  Neither the ViewDispatcher nor
 * the payload/conversation databases are safe to touch from there, so
 * anything needing GUI access or DB mutation is packaged into a
 * request and handed to the main thread via a custom event.  The
 * bridge then blocks on `done` until the main thread has finished.
 */

typedef enum {
    PifkReqNone = 0,
    PifkReqExecBadUsb,
    PifkReqExecNfc,
    PifkReqExecBle,
    PifkReqExecQr,
    PifkReqExecUsbDesc,
    PifkReqStopUsbDesc,
    PifkReqExecGpio,
    PifkReqExecGpioCapture,
    PifkReqExecI2c,
    PifkReqScanI2c,
    PifkReqExecBleGatt,
    PifkReqStopBleGatt,
    PifkReqExecNfcEmu,
    PifkReqExecNfcEmuUrl,
    PifkReqStopNfcEmu,
    PifkReqStopBle,
    PifkReqStopExec,
    PifkReqLoad,
    PifkReqReload,
} PifkRequestKind;

typedef struct {
    PifkRequestKind kind;
    char name[PIFK_MAX_NAME_LEN];
    char text[PIFK_MAX_TEXT_LEN];
    bool ok;
    char detail[128]; /* path / error message for the reply */
    FuriEventFlag* done;
} PifkRequest;

/* Custom event IDs.  Scene-local events use the low range; bridge
 * requests are offset so they can never collide with them. */
#define PIFK_EVENT_BRIDGE_REQUEST 0x1000

/* Event flag bit the bridge thread waits on and the main thread sets
 * once a request has been serviced. */
#define PIFK_REQUEST_DONE_FLAG (1u << 0)

/* ── App state ───────────────────────────────────────────────── */

typedef struct {
    /* Core */
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    NotificationApp* notifications;

    /* Views */
    /* Every list in the app.  Replaces the stock Submenu, which cannot
     * report a Right press without losing its own scrolling and has no
     * icon support; see gui/pifk_menu.h. */
    PifkMenu* menu;
    TextBox* text_box;
    VariableItemList* var_item_list;
    TextInput* text_input;
    Popup* popup;
    View* qr_view;

    /* Data.  Guarded by db_mutex whenever the bridge thread is running:
     * the GUI thread holds raw pointers into these arrays, so a
     * concurrent reload would free text out from under it. */
    PayloadDb* payload_db;
    FuriMutex* db_mutex;

    /* Execution state */
    bool executing;
    uint16_t selected_payload_index;

    /* Transport-first navigation: the group chosen on the main menu, and
     * the channel chosen within it. PifkChannelGroup and
     * PifkChannelId respectively, stored as plain integers so
     * pifk_app.h does not have to include channel.h — channel.h is
     * included by code that this header's consumers use. */
    uint8_t selected_group;
    uint8_t selected_channel;

    /* Polls bridge_state so Remote Mode can show it changing.  Held here
     * rather than in scene state so app teardown can disarm it: scene
     * on_exit handlers do not run when the app is freed. */
    FuriTimer* remote_refresh_timer;

    /* GPIO/UART egress settings, persisted in settings.json. */
    uint32_t gpio_baud; /* 9600 .. 921600 */
    uint8_t gpio_serial_id; /* FuriHalSerialId: Usart or Lpuart */
    uint8_t gpio_line_ending; /* PifkGpioLineEnding */
    uint32_t gpio_byte_delay_ms; /* 0 = burst; >0 paces and allows abort */
    uint32_t gpio_listen_ms; /* response-capture window */

    /* I2C egress settings, persisted in settings.json.  7-bit address in
     * conventional unshifted form; the HAL wants it shifted, which
     * i2c_exec.c does in one place. */
    uint8_t i2c_address; /* 0x08 .. 0x77 */

    /* Serial bridge */
    PifkBridgeState bridge_state;
    FuriStreamBuffer* serial_rx;
    FuriStreamBuffer* serial_tx;
    FuriThread* bridge_thread;
    char serial_buf[PIFK_SERIAL_BUF_SIZE];
    PifkRequest bridge_request;

    /* Settings */
    /* Delay after switching USB to HID, before typing starts, giving
     * the host time to enumerate the keyboard.  Per-keystroke timing is
     * fixed at 10ms in badusb_exec.c. */
    uint32_t badusb_delay_ms;

    /* BLE beacon state */
    bool ble_active;
    FuriTimer* ble_rotation_timer;
    FuriTimer* ble_duration_timer;
    uint8_t ble_chunk_index;
    uint8_t ble_chunk_count;
    char** ble_chunks;

    /* Text buffers */
    char text_buf[512];
    char input_buf[PIFK_MAX_NAME_LEN];
} PifkApp;

/* ── App lifecycle ───────────────────────────────────────────── */

PifkApp* pifk_app_alloc(void);
void pifk_app_free(PifkApp* app);

/* Service a bridge request on the main thread.  Called from the
 * view dispatcher's custom event callback only. */
void pifk_service_bridge_request(PifkApp* app);

/* ── Scene handler declarations (implemented per scene file) ── */

void pifk_scene_main_menu_on_enter(void* context);
bool pifk_scene_main_menu_on_event(void* context, SceneManagerEvent event);
void pifk_scene_main_menu_on_exit(void* context);

void pifk_scene_payload_list_on_enter(void* context);
bool pifk_scene_payload_list_on_event(void* context, SceneManagerEvent event);
void pifk_scene_payload_list_on_exit(void* context);

void pifk_scene_payload_view_on_enter(void* context);
bool pifk_scene_payload_view_on_event(void* context, SceneManagerEvent event);
void pifk_scene_payload_view_on_exit(void* context);

void pifk_scene_channel_list_on_enter(void* context);
bool pifk_scene_channel_list_on_event(void* context, SceneManagerEvent event);
void pifk_scene_channel_list_on_exit(void* context);

void pifk_scene_payload_pick_on_enter(void* context);
bool pifk_scene_payload_pick_on_event(void* context, SceneManagerEvent event);
void pifk_scene_payload_pick_on_exit(void* context);

void pifk_scene_remote_mode_on_enter(void* context);
bool pifk_scene_remote_mode_on_event(void* context, SceneManagerEvent event);
void pifk_scene_remote_mode_on_exit(void* context);

void pifk_scene_settings_on_enter(void* context);
bool pifk_scene_settings_on_event(void* context, SceneManagerEvent event);
void pifk_scene_settings_on_exit(void* context);

void pifk_scene_about_on_enter(void* context);
bool pifk_scene_about_on_event(void* context, SceneManagerEvent event);
void pifk_scene_about_on_exit(void* context);

void pifk_scene_captures_on_enter(void* context);
bool pifk_scene_captures_on_event(void* context, SceneManagerEvent event);
void pifk_scene_captures_on_exit(void* context);
