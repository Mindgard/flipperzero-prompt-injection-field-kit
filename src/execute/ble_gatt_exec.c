/*
 * BLE GATT payload surface.
 *
 * The Extra Beacon channel broadcasts 29 bytes at a time and hopes a
 * scanner is listening. This one publishes a readable service instead:
 * a client connects, reads a characteristic, and gets the whole payload.
 *
 * The firmware will not give a FAP its HID profile, but it will accept a
 * caller-supplied FuriHalBleProfileTemplate — three function pointers —
 * and the GATT construction API is fully exported. So the profile is
 * ours to define; we are not reimplementing anything the firmware
 * withheld, just using the primitives it offers.
 *
 * Two things need care:
 *
 *   1. Service teardown order. Characteristics must be deleted before
 *      the service that holds them, or the handles leak in the BLE
 *      stack's attribute table until reboot.
 *
 *   2. Profile ownership. bt_profile_start() replaces whatever profile
 *      was active, so bt_profile_restore_default() has to run on every
 *      exit path — including app teardown, where scene on_exit does not.
 */

#include "ble_gatt_exec.h"
#include <furi_ble/profile_interface.h>
#include <furi_ble/gatt.h>
#include <ble/core/ble_defs.h>
#include <furi_hal_bt.h>
#include <furi_hal_version.h>
#include <bt/bt_service/bt.h>
#include <string.h>

/* 16-bit UUIDs from the unassigned range. Deliberately not impersonating
 * a real vendor's service: the kit should be identifiable as itself. */
#define PIFK_GATT_SERVICE_UUID   0xFE20
#define PIFK_GATT_CHAR_BASE_UUID 0xFE21

/* Payload storage, served on read. Static because the GATT layer keeps
 * pointers to the characteristic data and reads them when a client
 * asks, long after the start call returns. */
static struct {
    uint8_t data[PIFK_GATT_CHAR_COUNT][PIFK_GATT_CHAR_MAX];
    uint16_t len[PIFK_GATT_CHAR_COUNT];
    uint8_t chars_used;
} pifk_gatt_payload;

/* Profile instance. The template contract requires the base struct to be
 * the first field so the firmware can identify which profile an instance
 * belongs to. */
typedef struct {
    FuriHalBleProfileBase base;
    uint16_t svc_handle;
    BleGattCharacteristicInstance chars[PIFK_GATT_CHAR_COUNT];
    BleGattCharacteristicParams char_params[PIFK_GATT_CHAR_COUNT];
    bool chars_valid[PIFK_GATT_CHAR_COUNT];
} PifkGattProfile;

static FuriHalBleProfileBase* pifk_gatt_profile_start(FuriHalBleProfileParams params);
static void pifk_gatt_profile_stop(FuriHalBleProfileBase* profile);
static void pifk_gatt_profile_gap_config(GapConfig* config, FuriHalBleProfileParams params);

static const FuriHalBleProfileTemplate pifk_gatt_profile_template = {
    .start = pifk_gatt_profile_start,
    .stop = pifk_gatt_profile_stop,
    .get_gap_config = pifk_gatt_profile_gap_config,
};

static FuriHalBleProfileBase* pifk_gatt_instance = NULL;
static Bt* pifk_gatt_bt = NULL;

/* ── Characteristic data callback ────────────────────────────── */

/* The characteristic index is carried in the context pointer, biased by
 * one so index 0 is not encoded as NULL.
 *
 * The bias is defensive rather than a fix for an observed bug: it was
 * tried as a theory for the first characteristic serving stale data and
 * changed nothing (the real cause was value length, see CHAR_MAX). It
 * stays because ble_gatt_characteristic_update() does
 * `if(source) context = source;` — a NULL context is indistinguishable
 * from "no context" to the layers in between, so encoding a real index
 * as NULL is a trap worth not setting. */
#define PIFK_GATT_CTX(index)     ((const void*)(uintptr_t)((index) + 1))
#define PIFK_GATT_CTX_INDEX(ctx) ((size_t)(uintptr_t)(ctx) - 1)

/* Called by the GATT layer twice per characteristic: once at init with
 * data == NULL to learn the maximum length, then on each update with a
 * context. Returning false means we keep ownership of the buffer. */
static bool pifk_gatt_char_data(const void* context, const uint8_t** data, uint16_t* data_len) {
    if(context == NULL) {
        /* Unreachable while every context is biased, but serving the
         * wrong buffer is worse than serving nothing. */
        *data_len = 0;
        return false;
    }

    size_t index = PIFK_GATT_CTX_INDEX(context);
    if(index >= PIFK_GATT_CHAR_COUNT) {
        *data_len = 0;
        return false;
    }
    if(data == NULL) {
        /* Length probe: advertise the ceiling, not the current payload,
         * so the characteristic can hold a later, longer payload without
         * being re-created. */
        *data_len = PIFK_GATT_CHAR_MAX;
        return false;
    }
    *data = pifk_gatt_payload.data[index];
    *data_len = pifk_gatt_payload.len[index];
    return false;
}

/* ── Profile implementation ──────────────────────────────────── */

static FuriHalBleProfileBase* pifk_gatt_profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);

    PifkGattProfile* profile = malloc(sizeof(PifkGattProfile));
    if(!profile) return NULL;
    memset(profile, 0, sizeof(PifkGattProfile));
    profile->base.config = &pifk_gatt_profile_template;

    Service_UUID_t svc_uuid = {.Service_UUID_16 = PIFK_GATT_SERVICE_UUID};

    /* Attribute budget: 1 for the service declaration plus 2 per
     * characteristic (declaration + value). Undersizing this makes
     * characteristic creation fail silently partway through. */
    uint8_t max_records = 1 + (PIFK_GATT_CHAR_COUNT * 2);

    if(!ble_gatt_service_add(
           UUID_TYPE_16, &svc_uuid, PRIMARY_SERVICE, max_records, &profile->svc_handle)) {
        free(profile);
        return NULL;
    }

    for(size_t i = 0; i < PIFK_GATT_CHAR_COUNT; i++) {
        profile->char_params[i] = (BleGattCharacteristicParams){
            .name = "Payload",
            .data_prop_type = FlipperGattCharacteristicDataCallback,
            .data.callback =
                {
                    .fn = pifk_gatt_char_data,
                    .context = PIFK_GATT_CTX(i),
                },
            .uuid.Char_UUID_16 = PIFK_GATT_CHAR_BASE_UUID + i,
            .uuid_type = UUID_TYPE_16,
            .char_properties = CHAR_PROP_READ,
            .security_permissions = ATTR_PERMISSION_NONE,
            .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
            .is_variable = CHAR_VALUE_LEN_VARIABLE,
        };
        ble_gatt_characteristic_init(
            profile->svc_handle, &profile->char_params[i], &profile->chars[i]);
        profile->chars_valid[i] = true;
        ble_gatt_characteristic_update(profile->svc_handle, &profile->chars[i], NULL);
    }

    return &profile->base;
}

static void pifk_gatt_profile_stop(FuriHalBleProfileBase* profile) {
    /* Contract requires checking the instance belongs to this profile:
     * the firmware may hand us someone else's. */
    furi_check(profile);
    if(profile->config != &pifk_gatt_profile_template) return;

    PifkGattProfile* p = (PifkGattProfile*)profile;

    /* Characteristics before the service, or their handles leak. */
    for(size_t i = 0; i < PIFK_GATT_CHAR_COUNT; i++) {
        if(p->chars_valid[i]) {
            ble_gatt_characteristic_delete(p->svc_handle, &p->chars[i]);
            p->chars_valid[i] = false;
        }
    }
    ble_gatt_service_delete(p->svc_handle);
    free(p);
}

static void pifk_gatt_profile_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    memset(config, 0, sizeof(GapConfig));

    /* The device's own BLE MAC. Zeroing the config and leaving this unset
     * is not a cosmetic omission: an all-zero address is not a valid BLE
     * device address, so the controller accepts the profile and then
     * never advertises. bt_profile_start() still returns a live instance,
     * which is how this managed to report "serving" with the radio
     * silent — nothing in the start path fails. */
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));

    /* Advertise the payload service so a scanner sees something worth
     * connecting to. */
    config->adv_service.UUID_Type = UUID_TYPE_16;
    config->adv_service.Service_UUID_16 = PIFK_GATT_SERVICE_UUID;

    /* No pairing: the point is that a client can read this without any
     * user interaction on either side. */
    config->pairing_method = GapPairingNone;
    config->bonding_mode = false;

    /* Identifiable as the kit rather than posing as a specific product.
     *
     * adv_name is not a plain string: it is handed to
     * aci_gap_set_discoverable() as a complete AD structure, so byte 0
     * must be the AD type and the name starts at byte 1 (gap.c does
     * `char* name = gap->service.adv_name + 1;`). Writing the name from
     * byte 0 feeds its first character to the controller as the type
     * byte, which drops that character from the advertised name. */
    config->adv_name[0] = AD_TYPE_COMPLETE_LOCAL_NAME;
    strlcpy(&config->adv_name[1], "PIFK", sizeof(config->adv_name) - 1);

    config->conn_param.conn_int_min = 0x18; /* 30 ms */
    config->conn_param.conn_int_max = 0x24; /* 45 ms */
    config->conn_param.slave_latency = 0;
    config->conn_param.supervisor_timeout = 0;
}

/* ── Public API ──────────────────────────────────────────────── */

uint8_t pifk_ble_gatt_chars_used(const PifkApp* app) {
    UNUSED(app);
    return pifk_gatt_payload.chars_used;
}

bool pifk_execute_ble_gatt(PifkApp* app, const PifkPayload* payload) {
    if(!payload || !payload->text || !payload->text[0]) return false;

    size_t len = strlen(payload->text);
    /* Refuse rather than serve a truncated prompt, consistent with the
     * beacon channel: a partial payload is a misleading result. */
    if(len > PIFK_GATT_TOTAL_BYTES) return false;

    /* Re-entry: tear down the previous service first so we do not leak
     * the profile instance. */
    if(pifk_gatt_instance) {
        pifk_ble_gatt_stop(app);
    }

    /* Split across characteristics. Done before starting the profile
     * because the data callback can fire as soon as it is up. */
    memset(&pifk_gatt_payload, 0, sizeof(pifk_gatt_payload));
    const char* cursor = payload->text;
    size_t remaining = len;
    for(size_t i = 0; i < PIFK_GATT_CHAR_COUNT && remaining > 0; i++) {
        size_t take = (remaining > PIFK_GATT_CHAR_MAX) ? PIFK_GATT_CHAR_MAX : remaining;
        memcpy(pifk_gatt_payload.data[i], cursor, take);
        pifk_gatt_payload.len[i] = (uint16_t)take;
        cursor += take;
        remaining -= take;
        pifk_gatt_payload.chars_used = (uint8_t)(i + 1);
    }

    /* Bluetooth can be switched off in the Flipper's own settings, and
     * bt_profile_start() still succeeds in that state — the profile is
     * registered, nothing ever reaches the air. Reporting success then
     * is the worst outcome for this kit: an operator concludes the
     * target ignored the payload when it was never transmitted. */
    if(!furi_hal_bt_is_active()) return false;

    if(!pifk_gatt_bt) {
        pifk_gatt_bt = furi_record_open(RECORD_BT);
    }

    pifk_gatt_instance = bt_profile_start(pifk_gatt_bt, &pifk_gatt_profile_template, NULL);
    if(!pifk_gatt_instance) {
        furi_record_close(RECORD_BT);
        pifk_gatt_bt = NULL;
        return false;
    }

    furi_hal_bt_start_advertising();
    return true;
}

void pifk_ble_gatt_stop(PifkApp* app) {
    UNUSED(app);
    if(!pifk_gatt_instance) return;

    furi_hal_bt_stop_advertising();

    /* Restoring the default profile is what actually tears ours down;
     * leaving it active would keep the Flipper serving the payload after
     * the user has left the scene. */
    if(pifk_gatt_bt) {
        bt_profile_restore_default(pifk_gatt_bt);
        furi_record_close(RECORD_BT);
        pifk_gatt_bt = NULL;
    }

    pifk_gatt_instance = NULL;
    memset(&pifk_gatt_payload, 0, sizeof(pifk_gatt_payload));
}
