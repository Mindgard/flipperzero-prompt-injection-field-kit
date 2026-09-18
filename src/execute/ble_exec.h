#pragma once

#include "../pifk_app.h"

/* Start broadcasting the payload text as BLE advertising data via the
 * Flipper's Extra Beacon API.
 *
 * Payloads of 29 bytes or fewer go out as a single static
 * advertisement.  Longer ones are split into "[N/M] <text>" chunks and
 * rotated every 2 seconds, which caps the payload at 2079 bytes
 * (99 chunks of 21 bytes).  Anything longer is refused rather than
 * truncated — a partial prompt on the air is a misleading result.
 *
 * Broadcasting runs for `duration_sec` seconds, or 60 if that is 0,
 * unless pifk_ble_abort() stops it first.
 *
 * Returns false if the payload is empty, too long, or the beacon
 * could not be configured. */
bool pifk_execute_ble(PifkApp* app, const PifkPayload* payload, uint32_t duration_sec);

/* Stop any active broadcast, cancel both timers and free the chunks.
 * Safe to call when nothing is running, and it also cleans up after a
 * pifk_execute_ble() that failed part-way through. */
void pifk_ble_abort(PifkApp* app);
