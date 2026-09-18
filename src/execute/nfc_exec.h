#pragma once

#include "../pifk_app.h"

/* Write an NDEF text record .nfc file for the given payload and
 * optionally launch the built-in NFC app to emulate it.
 *
 * When `launch_app` is true the Pifk app enqueues the NFC app
 * for launch (via loader_enqueue_launch) and signals our
 * view_dispatcher to stop so that the NFC app can take over.
 *
 * Returns true if the file was written successfully. */
bool pifk_execute_nfc(PifkApp* app, const PifkPayload* payload, bool launch_app);

/* Write an NDEF text record .nfc file and return the path in `out_path`.
 * Useful for the bridge protocol where we can't launch a GUI app.
 * Returns true on success. */
bool pifk_nfc_write_file(const PifkPayload* payload, char* out_path, size_t out_path_len);

/* Write an NDEF text-record .nfc file containing `text` to `path`. */
bool pifk_nfc_write_payload_file(const char* path, const char* text);
