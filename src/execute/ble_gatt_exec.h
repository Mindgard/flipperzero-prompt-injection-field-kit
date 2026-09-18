#pragma once

#include "../pifk_app.h"

/* Expose payload text as readable BLE GATT characteristics.
 *
 * The existing BLE channel (ble_exec.c) stuffs the payload into an
 * advertisement's Complete Local Name: 29 bytes per advertisement,
 * chunked with [N/M] prefixes and rotated every two seconds. A scanner
 * has to observe several rotations and reassemble them, and there is no
 * delivery guarantee — it is opportunistic broadcast.
 *
 * A GATT characteristic here is 244 bytes, addressable, and read on
 * demand rather than caught in passing. Three carry 732 bytes with no
 * chunking and no loss.
 *
 * Why this matters beyond capacity: BLE enumeration is something an
 * assistant with device-discovery or IoT-management capability does
 * routinely. A characteristic named like device metadata that returns
 * payload text is an ingestion point for any pipeline that enumerates
 * nearby devices and summarises them — the same shape as prompt
 * injection through automatically-extracted metadata, where the text
 * enters the model's context without the user asking for it or seeing
 * it.
 *
 * Unlike the firmware's HID profile, nothing here is withheld from
 * FAPs: ble_gatt_service_add() and ble_gatt_characteristic_init() are
 * both exported, so the profile is ours to define. */

/* Bytes per characteristic.
 *
 * Not a protocol ceiling — BleGattCharacteristicParams carries no
 * max_length field at all (that is on the descriptor params), and the
 * size comes from the callback's length probe, which is uint16_t.
 *
 * 244 because a value has to fit one ATT read. At 255 a real client got
 * the correct length back but stale buffer contents from the first
 * characteristic, while the 118-byte second one was perfect. Device-side
 * logging ruled out our side of it — the callback handed over the right
 * pointer and the right bytes ("We " at index 0) — so the corruption is
 * downstream, in the value the stack caches for reads that spill past a
 * single ATT exchange.
 *
 * 244 is ATT_MTU minus the 3-byte read response header, for the MTU of
 * 247 negotiated with macOS. The firmware supports up to 414
 * (CFG_BLE_MAX_ATT_MTU), so this is the client's number rather than the
 * device's: a client negotiating a smaller MTU would need a smaller
 * value still. Verified end to end at 244 — 373 bytes reassembled
 * byte-identical across two characteristics. */
#define PIFK_GATT_CHAR_MAX 244

/* Characteristics used to carry the payload. */
#define PIFK_GATT_CHAR_COUNT 3

#define PIFK_GATT_TOTAL_BYTES (PIFK_GATT_CHAR_MAX * PIFK_GATT_CHAR_COUNT)

/* Start advertising and serve `payload` from the GATT characteristics.
 *
 * Replaces the active BLE profile, so this cannot run alongside the
 * Extra Beacon channel — the caller is expected to stop that first.
 *
 * Returns false if the payload is empty, longer than
 * PIFK_GATT_TOTAL_BYTES, or the profile could not be started. */
bool pifk_execute_ble_gatt(PifkApp* app, const PifkPayload* payload);

/* Stop serving and restore the Flipper's default BLE profile. Safe to
 * call when nothing is running. */
void pifk_ble_gatt_stop(PifkApp* app);

/* Number of characteristics the payload occupies, for the UI. */
uint8_t pifk_ble_gatt_chars_used(const PifkApp* app);
