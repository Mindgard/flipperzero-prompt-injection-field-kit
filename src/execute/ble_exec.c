/*
 * BLE Extra Beacon advertising for payload delivery.
 *
 * The payload text is broadcast as the Complete Local Name of a
 * non-connectable advertisement, so anything scanning for nearby
 * devices sees it without connecting.
 *
 * An advertisement holds 29 bytes of name, which almost nothing fits
 * in, so longer text is split into "[N/M] " chunks that rotate every
 * two seconds.  A scanner therefore has to observe several rotations
 * and reassemble them; there is no ordering or delivery guarantee.
 * Treat this as opportunistic delivery, not a transport.
 */

#include "ble_exec.h"
#include <extra_beacon.h>
#include <furi_hal_bt.h>

/* Maximum usable bytes in a single advertisement.
 * BLE advertising PDU max is 31 bytes.  We use a Complete Local Name
 * AD structure: 1 byte length + 1 byte type (0x09) + up to 29 bytes. */
#define BLE_AD_NAME_MAX      29
#define BLE_ROTATION_MS      2000
#define BLE_DEFAULT_DURATION 60

/* Highest chunk number the "[N/M] " prefix can display in two digits. */
#define BLE_MAX_CHUNKS 99

/* ── Internal helpers ────────────────────────────────────────── */

/* Build a Complete Local Name AD structure in `out`.
 * Returns the total number of bytes written. */
static uint8_t build_ad_name(uint8_t* out, const char* name, uint8_t name_len) {
    if(name_len > BLE_AD_NAME_MAX) name_len = BLE_AD_NAME_MAX;
    out[0] = name_len + 1; /* Length: type byte + name bytes */
    out[1] = 0x09; /* AD type: Complete Local Name */
    memcpy(&out[2], name, name_len);
    return name_len + 2;
}

static void ble_free_chunks(PifkApp* app) {
    if(app->ble_chunks) {
        for(uint8_t i = 0; i < app->ble_chunk_count; i++) {
            free(app->ble_chunks[i]);
        }
        free(app->ble_chunks);
        app->ble_chunks = NULL;
    }
    app->ble_chunk_count = 0;
    app->ble_chunk_index = 0;
}

static bool ble_set_chunk(PifkApp* app, uint8_t index) {
    if(!app->ble_chunks || index >= app->ble_chunk_count) return false;

    const char* chunk = app->ble_chunks[index];
    if(!chunk) return false;
    uint8_t chunk_len = (uint8_t)strlen(chunk);

    /* build_ad_name() writes 2 header bytes + up to BLE_AD_NAME_MAX
     * name bytes, so the beacon buffer must hold at least that much. */
    _Static_assert(
        BLE_AD_NAME_MAX + 2 <= EXTRA_BEACON_MAX_DATA_SIZE,
        "AD name exceeds extra beacon data size");

    uint8_t ad_buf[EXTRA_BEACON_MAX_DATA_SIZE];
    uint8_t ad_len = build_ad_name(ad_buf, chunk, chunk_len);

    return furi_hal_bt_extra_beacon_set_data(ad_buf, ad_len);
}

/* ── Timer callback for chunk rotation ───────────────────────── */

static void ble_rotation_timer_cb(void* context) {
    PifkApp* app = context;
    if(!app->ble_active || app->ble_chunk_count <= 1) return;

    /* Stop beacon briefly to swap data */
    furi_hal_bt_extra_beacon_stop();

    app->ble_chunk_index = (app->ble_chunk_index + 1) % app->ble_chunk_count;
    ble_set_chunk(app, app->ble_chunk_index);

    furi_hal_bt_extra_beacon_start();
}

/* ── Duration timer callback ─────────────────────────────────── */

static void ble_duration_timer_cb(void* context) {
    PifkApp* app = context;
    pifk_ble_abort(app);
    notification_message(app->notifications, &sequence_blink_stop);
}

/* ── Public API ──────────────────────────────────────────────── */

bool pifk_execute_ble(PifkApp* app, const PifkPayload* payload, uint32_t duration_sec) {
    if(!payload || !payload->text || !payload->text[0]) return false;

    /* Bluetooth off in the Flipper's settings means the name changes
     * below go nowhere. Fail rather than report a broadcast that never
     * left the device. */
    if(!furi_hal_bt_is_active()) return false;

    /* Clean up any previous BLE state */
    pifk_ble_abort(app);

    size_t text_len = strlen(payload->text);

    if(text_len <= BLE_AD_NAME_MAX) {
        /* Short payload: single static advertisement */
        app->ble_chunks = malloc(sizeof(char*));
        if(!app->ble_chunks) return false;
        app->ble_chunks[0] = strdup(payload->text);
        if(!app->ble_chunks[0]) {
            free(app->ble_chunks);
            app->ble_chunks = NULL;
            return false;
        }
        app->ble_chunk_count = 1;
    } else {
        /* Split into chunks prefixed with "[N/M] ".  The prefix grows
         * with the chunk count, so size it from the widest value we
         * could print rather than from a first-pass estimate. */
        char prefix_test[16];
        snprintf(prefix_test, sizeof(prefix_test), "[%d/%d] ", BLE_MAX_CHUNKS, BLE_MAX_CHUNKS);
        uint8_t prefix_len = (uint8_t)strlen(prefix_test);
        uint8_t text_per_chunk = BLE_AD_NAME_MAX - prefix_len;

        /* Refuse rather than silently broadcast a truncated prompt: for
         * a payload-fidelity tool, a partial payload is a wrong result. */
        size_t max_len = (size_t)text_per_chunk * BLE_MAX_CHUNKS;
        if(text_len > max_len) return false;

        uint8_t total_chunks = (uint8_t)((text_len + text_per_chunk - 1) / text_per_chunk);

        app->ble_chunks = malloc(sizeof(char*) * total_chunks);
        if(!app->ble_chunks) return false;
        memset(app->ble_chunks, 0, sizeof(char*) * total_chunks);
        app->ble_chunk_count = total_chunks;

        const char* src = payload->text;
        size_t remaining = text_len;

        for(uint8_t i = 0; i < total_chunks; i++) {
            char chunk_buf[BLE_AD_NAME_MAX + 1];
            uint8_t copy_len = (remaining < text_per_chunk) ? (uint8_t)remaining : text_per_chunk;

            int pn = snprintf(chunk_buf, sizeof(chunk_buf), "[%d/%d] ", i + 1, total_chunks);
            size_t prefix_actual = (pn > 0) ? (size_t)pn : 0;
            /* prefix_actual + copy_len <= BLE_AD_NAME_MAX by construction */
            memcpy(chunk_buf + prefix_actual, src, copy_len);
            chunk_buf[prefix_actual + copy_len] = '\0';

            app->ble_chunks[i] = strdup(chunk_buf);
            if(!app->ble_chunks[i]) {
                ble_free_chunks(app);
                return false;
            }
            src += copy_len;
            remaining -= copy_len;
        }
    }

    app->ble_chunk_index = 0;

    /* Configure the Extra Beacon */
    GapExtraBeaconConfig config = {
        .min_adv_interval_ms = 100,
        .max_adv_interval_ms = 200,
        .adv_channel_map = GapAdvChannelMapAll,
        .adv_power_level = GapAdvPowerLevel_0dBm,
        .address_type = GapAddressTypeRandom,
        .address = {0xDE, 0xAD, 0xBE, 0xEF, 0x13, 0x37},
    };

    if(!furi_hal_bt_extra_beacon_set_config(&config)) {
        ble_free_chunks(app);
        return false;
    }

    /* Set initial chunk data */
    if(!ble_set_chunk(app, 0)) {
        ble_free_chunks(app);
        return false;
    }

    /* Start broadcasting */
    if(!furi_hal_bt_extra_beacon_start()) {
        ble_free_chunks(app);
        return false;
    }

    app->ble_active = true;

    /* Set up rotation timer for multi-chunk payloads */
    if(app->ble_chunk_count > 1) {
        app->ble_rotation_timer =
            furi_timer_alloc(ble_rotation_timer_cb, FuriTimerTypePeriodic, app);
        furi_timer_start(app->ble_rotation_timer, BLE_ROTATION_MS);
    }

    /* Set up duration timer */
    uint32_t dur = (duration_sec > 0) ? duration_sec : BLE_DEFAULT_DURATION;
    app->ble_duration_timer = furi_timer_alloc(ble_duration_timer_cb, FuriTimerTypeOnce, app);
    furi_timer_start(app->ble_duration_timer, dur * 1000);

    return true;
}

void pifk_ble_abort(PifkApp* app) {
    /* Not active but chunks allocated means a failed start left state
     * behind; fall through so it still gets released. */
    if(!app->ble_active && !app->ble_chunks && !app->ble_rotation_timer &&
       !app->ble_duration_timer) {
        return;
    }

    app->ble_active = false;

    /* Stop timers */
    if(app->ble_rotation_timer) {
        furi_timer_stop(app->ble_rotation_timer);
        furi_timer_free(app->ble_rotation_timer);
        app->ble_rotation_timer = NULL;
    }
    if(app->ble_duration_timer) {
        furi_timer_stop(app->ble_duration_timer);
        furi_timer_free(app->ble_duration_timer);
        app->ble_duration_timer = NULL;
    }

    /* Stop beacon */
    furi_hal_bt_extra_beacon_stop();

    /* Free chunks */
    ble_free_chunks(app);
}
