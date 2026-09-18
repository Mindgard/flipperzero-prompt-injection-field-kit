/*
 * Prompt Injection Field Kit — Entry point & lifecycle.
 *
 * Native FAP that turns the Flipper into a standalone AI prompt injection
 * field kit, with a serial bridge mode for real-time control from a host
 * over USB serial.
 */

#include "pifk_app.h"
#include "channel/channel.h"
#include "payload/payload_db.h"
#include "payload/capture_log.h"
#include "serial/bridge_protocol.h"
#include "execute/badusb_exec.h"
#include "execute/ble_exec.h"
#include "execute/nfc_exec.h"
#include "execute/qr_exec.h"
#include "execute/qrcode.h"
#include "execute/usb_descriptor_exec.h"
#include "execute/gpio_exec.h"
#include "execute/i2c_exec.h"
#include "execute/ble_gatt_exec.h"
#include "execute/nfc_listener_exec.h"

/* ── Scene handler tables ────────────────────────────────────── */

static void (*const scene_on_enter_handlers[])(void*) = {
    [PifkSceneMainMenu] = pifk_scene_main_menu_on_enter,
    [PifkScenePayloadList] = pifk_scene_payload_list_on_enter,
    [PifkScenePayloadView] = pifk_scene_payload_view_on_enter,
    [PifkSceneChannelList] = pifk_scene_channel_list_on_enter,
    [PifkScenePayloadPick] = pifk_scene_payload_pick_on_enter,
    [PifkSceneRemoteMode] = pifk_scene_remote_mode_on_enter,
    [PifkSceneSettings] = pifk_scene_settings_on_enter,
    [PifkSceneAbout] = pifk_scene_about_on_enter,
    [PifkSceneCaptures] = pifk_scene_captures_on_enter,
};

static bool (*const scene_on_event_handlers[])(void*, SceneManagerEvent) = {
    [PifkSceneMainMenu] = pifk_scene_main_menu_on_event,
    [PifkScenePayloadList] = pifk_scene_payload_list_on_event,
    [PifkScenePayloadView] = pifk_scene_payload_view_on_event,
    [PifkSceneChannelList] = pifk_scene_channel_list_on_event,
    [PifkScenePayloadPick] = pifk_scene_payload_pick_on_event,
    [PifkSceneRemoteMode] = pifk_scene_remote_mode_on_event,
    [PifkSceneSettings] = pifk_scene_settings_on_event,
    [PifkSceneAbout] = pifk_scene_about_on_event,
    [PifkSceneCaptures] = pifk_scene_captures_on_event,
};

static void (*const scene_on_exit_handlers[])(void*) = {
    [PifkSceneMainMenu] = pifk_scene_main_menu_on_exit,
    [PifkScenePayloadList] = pifk_scene_payload_list_on_exit,
    [PifkScenePayloadView] = pifk_scene_payload_view_on_exit,
    [PifkSceneChannelList] = pifk_scene_channel_list_on_exit,
    [PifkScenePayloadPick] = pifk_scene_payload_pick_on_exit,
    [PifkSceneRemoteMode] = pifk_scene_remote_mode_on_exit,
    [PifkSceneSettings] = pifk_scene_settings_on_exit,
    [PifkSceneAbout] = pifk_scene_about_on_exit,
    [PifkSceneCaptures] = pifk_scene_captures_on_exit,
};

/* These are designated-initializer arrays, so a scene added to the enum
 * without a handler here becomes a NULL entry the scene manager calls.
 * Sizing is implicit from the highest index, which is exactly what makes
 * the omission silent — assert it instead.  Removing the sequence and
 * results scenes is what prompted this. */
_Static_assert(
    COUNT_OF(scene_on_enter_handlers) == PifkSceneCount,
    "an on_enter handler is missing for one of the scenes");
_Static_assert(
    COUNT_OF(scene_on_event_handlers) == PifkSceneCount,
    "an on_event handler is missing for one of the scenes");
_Static_assert(
    COUNT_OF(scene_on_exit_handlers) == PifkSceneCount,
    "an on_exit handler is missing for one of the scenes");

static const SceneManagerHandlers scene_handlers = {
    .on_enter_handlers = scene_on_enter_handlers,
    .on_event_handlers = scene_on_event_handlers,
    .on_exit_handlers = scene_on_exit_handlers,
    .scene_num = PifkSceneCount,
};

/* ── View dispatcher callbacks ───────────────────────────────── */

static bool pifk_app_back_event_cb(void* context) {
    PifkApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static bool pifk_app_custom_event_cb(void* context, uint32_t event) {
    PifkApp* app = context;
    if(event == PIFK_EVENT_BRIDGE_REQUEST) {
        pifk_service_bridge_request(app);
        return true;
    }
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

/* ── Bridge request servicing (main thread only) ──────────────── *
 *
 * The bridge thread parses a command, fills app->bridge_request and
 * posts PIFK_EVENT_BRIDGE_REQUEST, then blocks.  Everything below
 * therefore runs on the main thread, where touching the databases and
 * the ViewDispatcher is safe.
 */

void pifk_service_bridge_request(PifkApp* app) {
    PifkRequest* req = &app->bridge_request;
    req->ok = false;

    switch(req->kind) {
    case PifkReqExecBadUsb: {
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        const PifkPayload* p = payload_db_find(app->payload_db, req->name);
        if(!p) {
            furi_mutex_release(app->db_mutex);
            snprintf(req->detail, sizeof(req->detail), "payload not found: %.80s", req->name);
            break;
        }
        app->bridge_state = PifkBridgeExecuting;
        req->ok = pifk_execute_badusb(app, p);
        app->bridge_state = PifkBridgeConnected;
        furi_mutex_release(app->db_mutex);
        break;
    }

    case PifkReqExecNfc: {
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        const PifkPayload* p = payload_db_find(app->payload_db, req->name);
        if(!p) {
            furi_mutex_release(app->db_mutex);
            snprintf(req->detail, sizeof(req->detail), "payload not found: %.80s", req->name);
            break;
        }
        char nfc_path[128];
        req->ok = pifk_nfc_write_file(p, nfc_path, sizeof(nfc_path));
        furi_mutex_release(app->db_mutex);
        if(req->ok) {
            strlcpy(req->detail, nfc_path, sizeof(req->detail));
        } else {
            strlcpy(req->detail, "failed to write NFC file", sizeof(req->detail));
        }
        break;
    }

    case PifkReqExecBle: {
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        const PifkPayload* p = payload_db_find(app->payload_db, req->name);
        if(!p) {
            furi_mutex_release(app->db_mutex);
            snprintf(req->detail, sizeof(req->detail), "payload not found: %.80s", req->name);
            break;
        }
        req->ok = pifk_execute_ble(app, p, 0);
        furi_mutex_release(app->db_mutex);
        if(!req->ok) {
            strlcpy(req->detail, "failed to start BLE beacon", sizeof(req->detail));
        }
        break;
    }

    case PifkReqExecQr: {
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        const PifkPayload* p = payload_db_find(app->payload_db, req->name);
        if(!p) {
            furi_mutex_release(app->db_mutex);
            snprintf(req->detail, sizeof(req->detail), "payload not found: %.80s", req->name);
            break;
        }
        req->ok = pifk_execute_qr(app, p);
        furi_mutex_release(app->db_mutex);
        if(!req->ok) {
            snprintf(
                req->detail,
                sizeof(req->detail),
                "text too long for QR (max %d bytes)",
                PIFK_QR_MAX_BYTES);
        }
        break;
    }

    case PifkReqExecUsbDesc: {
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        const PifkPayload* p = payload_db_find(app->payload_db, req->name);
        if(!p) {
            furi_mutex_release(app->db_mutex);
            snprintf(req->detail, sizeof(req->detail), "payload not found: %.80s", req->name);
            break;
        }
        size_t lossy = pifk_usb_descriptor_lossy_chars(p->text);
        req->ok = pifk_execute_usb_descriptor(app, p);
        furi_mutex_release(app->db_mutex);

        if(!req->ok) {
            strlcpy(req->detail, "USB mode switch refused", sizeof(req->detail));
        } else if(lossy > 0) {
            /* Report the loss rather than claiming a clean send: the
             * host will log a truncated payload. */
            snprintf(
                req->detail,
                sizeof(req->detail),
                "%.60s (%u chars dropped)",
                req->name,
                (unsigned)lossy);
        }
        break;
    }

    case PifkReqExecGpio: {
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        const PifkPayload* p = payload_db_find(app->payload_db, req->name);
        if(!p) {
            furi_mutex_release(app->db_mutex);
            snprintf(req->detail, sizeof(req->detail), "payload not found: %.80s", req->name);
            break;
        }
        app->bridge_state = PifkBridgeExecuting;
        req->ok = pifk_execute_gpio(app, p);
        app->bridge_state = PifkBridgeConnected;
        furi_mutex_release(app->db_mutex);
        if(!req->ok) {
            const char* err = pifk_gpio_last_error();
            strlcpy(req->detail, err ? err : "GPIO send failed", sizeof(req->detail));
        }
        break;
    }

    case PifkReqExecGpioCapture: {
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        const PifkPayload* p = payload_db_find(app->payload_db, req->name);
        if(!p) {
            furi_mutex_release(app->db_mutex);
            snprintf(req->detail, sizeof(req->detail), "payload not found: %.80s", req->name);
            break;
        }
        app->bridge_state = PifkBridgeExecuting;
        req->ok = pifk_execute_gpio_capture(app, p, app->gpio_listen_ms);
        app->bridge_state = PifkBridgeConnected;
        /* Read the payload length while the lock is still held: p points
         * into the database, and a concurrent RELOAD would free its text. */
        size_t sent_len = strlen(p->text);
        furi_mutex_release(app->db_mutex);

        if(!req->ok) {
            const char* err = pifk_gpio_last_error();
            strlcpy(req->detail, err ? err : "GPIO send failed", sizeof(req->detail));
            break;
        }

        /* Report the reply itself, not just that we sent something.
         * detail is bounded, so a long reply is reported by length and
         * read from the device instead. */
        size_t rx_len = 0;
        const char* rx = pifk_gpio_response(&rx_len);

        /* Log it, so a capture driven from the host leaves the same
         * evidence on the device as one driven from the UI.  req->name is
         * a copy, so this is safe with the lock released. */
        capture_log_record(
            req->name, "gpio", sent_len, rx, rx_len, pifk_gpio_response_truncated());

        if(rx_len == 0) {
            snprintf(req->detail, sizeof(req->detail), "%.60s (no reply)", req->name);
        } else if(rx_len < sizeof(req->detail) - 24) {
            snprintf(req->detail, sizeof(req->detail), "%.60s: %s", req->name, rx);
        } else {
            snprintf(
                req->detail,
                sizeof(req->detail),
                "%.60s (%u bytes, see device)",
                req->name,
                (unsigned)rx_len);
        }
        break;
    }

    case PifkReqScanI2c: {
        PifkI2cScan scan;
        app->bridge_state = PifkBridgeExecuting;
        req->ok = pifk_i2c_scan(app, &scan);
        app->bridge_state = PifkBridgeConnected;

        if(!req->ok) {
            const char* err = pifk_i2c_last_error();
            strlcpy(req->detail, err ? err : "I2C scan failed", sizeof(req->detail));
            break;
        }

        /* List the addresses found, so a host can pick one without a
         * second round trip. */
        int n = snprintf(req->detail, sizeof(req->detail), "%u:", (unsigned)scan.total);
        for(uint8_t i = 0; i < scan.count && n > 0 && n < (int)sizeof(req->detail); i++) {
            n += snprintf(
                req->detail + n, sizeof(req->detail) - (size_t)n, " 0x%02X", scan.addr[i]);
        }
        break;
    }

    case PifkReqExecI2c: {
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        const PifkPayload* p = payload_db_find(app->payload_db, req->name);
        if(!p) {
            furi_mutex_release(app->db_mutex);
            snprintf(req->detail, sizeof(req->detail), "payload not found: %.80s", req->name);
            break;
        }
        app->bridge_state = PifkBridgeExecuting;
        req->ok = pifk_i2c_execute(app, p);
        app->bridge_state = PifkBridgeConnected;
        furi_mutex_release(app->db_mutex);

        /* Report the byte count either way: a failed I2C write may still
         * have delivered part of the payload, and it cannot be undone. */
        if(!req->ok) {
            const char* err = pifk_i2c_last_error();
            snprintf(
                req->detail,
                sizeof(req->detail),
                "%.90s (%u bytes written)",
                err ? err : "I2C write failed",
                (unsigned)pifk_i2c_bytes_written());
            break;
        }
        snprintf(
            req->detail,
            sizeof(req->detail),
            "%.60s: %u bytes to 0x%02X",
            req->name,
            (unsigned)pifk_i2c_bytes_written(),
            app->i2c_address);
        break;
    }

    case PifkReqExecBleGatt: {
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        const PifkPayload* p = payload_db_find(app->payload_db, req->name);
        if(!p) {
            furi_mutex_release(app->db_mutex);
            snprintf(req->detail, sizeof(req->detail), "payload not found: %.80s", req->name);
            break;
        }
        /* Serving GATT replaces the active BLE profile, so the beacon
         * cannot run at the same time. */
        if(app->ble_active) pifk_ble_abort(app);
        req->ok = pifk_execute_ble_gatt(app, p);
        furi_mutex_release(app->db_mutex);
        if(!req->ok) {
            snprintf(
                req->detail,
                sizeof(req->detail),
                "GATT start failed (max %d bytes)",
                PIFK_GATT_TOTAL_BYTES);
        }
        break;
    }

    case PifkReqStopBleGatt:
        pifk_ble_gatt_stop(app);
        req->ok = true;
        break;

    case PifkReqExecNfcEmu:
    case PifkReqExecNfcEmuUrl: {
        const bool as_url = (req->kind == PifkReqExecNfcEmuUrl);
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        const PifkPayload* p = payload_db_find(app->payload_db, req->name);
        if(!p) {
            furi_mutex_release(app->db_mutex);
            snprintf(req->detail, sizeof(req->detail), "payload not found: %.80s", req->name);
            break;
        }
        req->ok = pifk_execute_nfc_listener(app, p, as_url ? PifkNfcRecordUri : PifkNfcRecordText);
        furi_mutex_release(app->db_mutex);
        if(!req->ok) {
            /* URL-encoding costs up to 3 bytes a character, so the URI
             * form runs out well before the text form does. Separate
             * calls because only one of these takes an argument. */
            if(as_url) {
                snprintf(
                    req->detail,
                    sizeof(req->detail),
                    "NFC URL emulation failed (too long once encoded, or NFC busy)");
            } else {
                snprintf(
                    req->detail,
                    sizeof(req->detail),
                    "NFC emulation failed (max %d bytes)",
                    PIFK_NFC_MAX_TEXT);
            }
        }
        break;
    }

    case PifkReqStopNfcEmu:
        pifk_nfc_listener_stop(app);
        req->ok = true;
        break;

    case PifkReqStopUsbDesc:
        pifk_usb_descriptor_stop(app);
        req->ok = true;
        break;

    case PifkReqStopBle:
        pifk_ble_abort(app);
        req->ok = true;
        break;

    case PifkReqStopExec:
        pifk_badusb_abort(app);
        req->ok = true;
        break;

    case PifkReqLoad: {
        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        PayloadDb* db = app->payload_db;
        if(db->payload_count >= PIFK_MAX_PAYLOADS) {
            strlcpy(req->detail, "payload database full", sizeof(req->detail));
        } else if(payload_db_find(db, req->name)) {
            /* Reject duplicates: payload_db_find() returns the first
             * match, so a second entry with the same name would be
             * unreachable and just consume a slot. */
            snprintf(req->detail, sizeof(req->detail), "payload already exists: %.80s", req->name);
        } else {
            char* dup = strdup(req->text);
            if(!dup) {
                strlcpy(req->detail, "out of memory", sizeof(req->detail));
            } else {
                PifkPayload* pl = &db->payloads[db->payload_count];
                memset(pl, 0, sizeof(*pl));
                strlcpy(pl->name, req->name, sizeof(pl->name));
                strlcpy(pl->category, "remote", sizeof(pl->category));
                pl->text = dup;
                pl->text_owned = true;
                pl->is_builtin = false;
                db->payload_count++;
                req->ok = true;
            }
        }
        furi_mutex_release(app->db_mutex);
        break;
    }

    case PifkReqReload: {
        /* A reload frees every heap payload text.  Any scene currently
         * displaying one holds a pointer into that memory, so unwind to
         * the main menu before swapping the data out from under it. */
        scene_manager_search_and_switch_to_another_scene(app->scene_manager, PifkSceneMainMenu);

        furi_mutex_acquire(app->db_mutex, FuriWaitForever);
        pifk_reload_data(app);
        snprintf(req->detail, sizeof(req->detail), "payloads=%u", app->payload_db->payload_count);
        furi_mutex_release(app->db_mutex);
        req->ok = true;
        break;
    }

    case PifkReqNone:
    default:
        strlcpy(req->detail, "no request pending", sizeof(req->detail));
        break;
    }

    req->kind = PifkReqNone;
    if(req->done) {
        furi_event_flag_set(req->done, PIFK_REQUEST_DONE_FLAG);
    }
}

/* ── Alloc / Free ────────────────────────────────────────────── */

PifkApp* pifk_app_alloc(void) {
    /* Launch is the tightest moment for heap in this app: the GUI views,
     * the payload database and the payloads.json read are all live at
     * once.  An unchecked malloc here would memset() through a NULL and
     * turn "no memory to start" into a hard fault, which is how this
     * presented in the field — a crash on launch rather than an error. */
    PifkApp* app = malloc(sizeof(PifkApp));
    if(!app) return NULL;
    memset(app, 0, sizeof(PifkApp));

    /* Defaults */
    app->badusb_delay_ms = 1000;

    app->bridge_state = PifkBridgeIdle;

    /* GPIO defaults: 115200 8N1 on the USART, newline-terminated.
     * LPUART avoids contention with the CLI but USART is the pin pair
     * users expect, so start there and let Settings switch. */
    app->gpio_baud = 115200;
    app->gpio_serial_id = (uint8_t)FuriHalSerialIdUsart;
    app->gpio_line_ending = (uint8_t)PifkGpioLineEndingLf;
    app->gpio_byte_delay_ms = 0;
    app->gpio_listen_ms = 1000;

    /* 0x50 is the conventional base address for a 24Cxx-series EEPROM,
     * which is the most likely thing on an unknown bus to accept a text
     * write without side effects. Deliberately not a display or PMIC
     * address: the write path probes before writing, but a sensible
     * default still matters if someone skips the scan. */
    app->i2c_address = 0x50;

    /* Core GUI */
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->scene_manager = scene_manager_alloc(&scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, pifk_app_custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, pifk_app_back_event_cb);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    /* Allocate views */
    app->menu = pifk_menu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, PifkViewMenu, pifk_menu_get_view(app->menu));

    app->text_box = text_box_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, PifkViewTextBox, text_box_get_view(app->text_box));

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        PifkViewVariableItemList,
        variable_item_list_get_view(app->var_item_list));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, PifkViewTextInput, text_input_get_view(app->text_input));

    app->popup = popup_alloc();
    view_dispatcher_add_view(app->view_dispatcher, PifkViewPopup, popup_get_view(app->popup));

    pifk_qr_view_alloc(app);

    /* Data stores.  ~12 KB of fixed inline array; see PIFK_MAX_PAYLOADS.
     * This is the single largest allocation the app makes, so it is the
     * one most likely to fail — report it rather than fault on it. */
    app->payload_db = malloc(sizeof(PayloadDb));
    if(!app->payload_db) {
        pifk_app_free(app);
        return NULL;
    }
    memset(app->payload_db, 0, sizeof(PayloadDb));

    /* Guards the databases against concurrent access from the bridge
     * thread while the GUI thread is rendering from them. */
    app->db_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    /* Serial bridge buffers */
    app->serial_rx = furi_stream_buffer_alloc(PIFK_SERIAL_BUF_SIZE, 1);
    app->serial_tx = furi_stream_buffer_alloc(PIFK_SERIAL_BUF_SIZE, 1);

    /* Load data from SD card */
    payload_db_load_builtins(app->payload_db);
    payload_db_load_from_file(app->payload_db, PIFK_PAYLOADS_FILE);
    favorites_load(app->payload_db, PIFK_FAVORITES_FILE);

    /* Seed an editable payloads.json on first run, detected by the file
     * not existing yet.  This writes only user payloads — of which there
     * are none on a first run, so it emits a short schema stub.  It used
     * to dump all 54 builtins, producing a 15 KB file that every later
     * launch read back into heap only for the loader to discard by name;
     * on a device with ~37 KB free that was most of the OOM headroom. */
    {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        if(!storage_file_exists(storage, PIFK_PAYLOADS_FILE)) {
            payload_db_export_user(app->payload_db, PIFK_PAYLOADS_FILE);
        }
        furi_record_close(RECORD_STORAGE);
    }

    /* Restore persisted settings (delays, default channel, GPIO, I2C) */
    settings_load(app, PIFK_SETTINGS_FILE);

    return app;
}

void pifk_app_free(PifkApp* app) {
    furi_assert(app);

    /* Scene on_exit handlers do not run during teardown, so a timer left
     * armed here would fire into freed memory.  Kill it first. */
    if(app->remote_refresh_timer) {
        furi_timer_stop(app->remote_refresh_timer);
        furi_timer_free(app->remote_refresh_timer);
        app->remote_refresh_timer = NULL;
    }
    /* Stop BLE beacon if active */
    pifk_ble_abort(app);

    /* Restore USB if a descriptor payload is still being advertised. */
    pifk_usb_descriptor_stop(app);

    /* Restore the default BLE profile if we are still serving GATT. */
    pifk_ble_gatt_stop(app);

    /* Release NFC if a tag is still being emulated. */
    pifk_nfc_listener_stop(app);

    /* Stop bridge thread if running */
    if(app->bridge_thread) {
        bridge_stop(app);
    }

    /* Free views */
    view_dispatcher_remove_view(app->view_dispatcher, PifkViewMenu);
    pifk_menu_free(app->menu);

    view_dispatcher_remove_view(app->view_dispatcher, PifkViewTextBox);
    text_box_free(app->text_box);

    view_dispatcher_remove_view(app->view_dispatcher, PifkViewVariableItemList);
    variable_item_list_free(app->var_item_list);

    view_dispatcher_remove_view(app->view_dispatcher, PifkViewTextInput);
    text_input_free(app->text_input);

    view_dispatcher_remove_view(app->view_dispatcher, PifkViewPopup);
    popup_free(app->popup);

    pifk_qr_view_free(app);

    /* Free core */
    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    /* Free data (release owned heap strings first).
     *
     * These are NULL-guarded because pifk_app_alloc() calls this to unwind
     * a partially-constructed app when an allocation fails: the views
     * above are already in place by then, but the data stores may not be. */
    if(app->payload_db) {
        payload_db_free_entries(app->payload_db);
        free(app->payload_db);
    }
    if(app->db_mutex) furi_mutex_free(app->db_mutex);

    /* Free serial buffers */
    if(app->serial_rx) furi_stream_buffer_free(app->serial_rx);
    if(app->serial_tx) furi_stream_buffer_free(app->serial_tx);

    /* Close records */
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);

    free(app);
}

/* ── Entry point ─────────────────────────────────────────────── */

int32_t pifk_app_main(void* p) {
    UNUSED(p);

    PifkApp* app = pifk_app_alloc();
    if(!app) {
        /* Out of heap before the GUI existed, so there is no view to show
         * an error in.  Log it and exit non-zero: the launcher reports a
         * failed app, which beats faulting on a NULL scene manager. */
        FURI_LOG_E("pifk", "not enough free heap to start");
        return -1;
    }

    scene_manager_next_scene(app->scene_manager, PifkSceneMainMenu);
    view_dispatcher_run(app->view_dispatcher);

    pifk_app_free(app);
    return 0;
}
