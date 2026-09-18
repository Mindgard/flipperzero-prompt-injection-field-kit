/*
 * Saved-tag generation for NFC payload delivery.
 *
 * Despite the filename, this module does not emulate anything.  It
 * writes .nfc files to the SD card and hands them to the Flipper's
 * built-in NFC app via loader_enqueue_launch(), which does the actual
 * emulation.  See nfc_listener_exec.c for the in-app path that emulates
 * without leaving the field kit.
 *
 * The practical consequence is that emulation replaces this app rather
 * than running alongside it: pifk_execute_nfc() stops our view
 * dispatcher so the loader can start the NFC app.
 *
 * The tag written here is the same NTAG215 image the in-app listener
 * emulates, so it behaves the same whichever route delivered it.
 */

#include "../pifk_app.h"
#include "nfc_exec.h"
#include "nfc_listener_exec.h"
#include <nfc/nfc_device.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <loader/loader.h>

/* ── Public API ──────────────────────────────────────────────── */

bool pifk_nfc_write_payload_file(const char* path, const char* text) {
    /* This used to emit raw NDEF bytes, which the stock NFC app cannot
     * read — it reported "cannot load key file" and emulation never
     * started. A .nfc file is a Flipper Format text document listing the
     * device type, UID and page contents, not a dump of tag memory.
     *
     * nfc_device_save() writes that format for us, so the file is right
     * by construction and stays right if upstream changes it. */
    if(!path || !text) return false;

    MfUltralightData* data = mf_ultralight_alloc();
    if(!data) return false;

    NfcDevice* device = NULL;
    bool ok = false;

    if(pifk_nfc_build_tag(data, text)) {
        device = nfc_device_alloc();
        if(device) {
            nfc_device_set_data(device, NfcProtocolMfUltralight, data);
            ok = nfc_device_save(device, path);
        }
    }

    if(device) nfc_device_free(device);
    mf_ultralight_free(data);
    return ok;
}

/* ── NFC execution via file + built-in app launch ────────────── */

#define NFC_FILE_DIR "/ext/nfc"
#define NFC_FILE_FMT NFC_FILE_DIR "/pifk_%s.nfc"

bool pifk_nfc_write_file(const PifkPayload* payload, char* out_path, size_t out_path_len) {
    /* Sanitise the name for the filesystem.  Payload names reach us
     * from SD-card JSON and the serial bridge, so allow only an
     * explicit safe set — a blocklist would let "..", backslashes and
     * control characters through into the path. */
    char safe_name[PIFK_MAX_NAME_LEN];
    size_t i;
    for(i = 0; i < PIFK_MAX_NAME_LEN - 1 && payload->name[i]; i++) {
        unsigned char c = (unsigned char)payload->name[i];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_';
        safe_name[i] = allowed ? (char)c : '_';
    }
    safe_name[i] = '\0';

    /* An all-separator name would collapse to an empty component. */
    if(safe_name[0] == '\0') {
        strlcpy(safe_name, "payload", sizeof(safe_name));
    }

    snprintf(out_path, out_path_len, NFC_FILE_FMT, safe_name);

    /* Ensure /ext/nfc directory exists */
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, NFC_FILE_DIR);
    furi_record_close(RECORD_STORAGE);

    return pifk_nfc_write_payload_file(out_path, payload->text);
}

bool pifk_execute_nfc(PifkApp* app, const PifkPayload* payload, bool launch_app) {
    char nfc_path[128];
    bool ok = pifk_nfc_write_file(payload, nfc_path, sizeof(nfc_path));
    if(!ok) {
        return false;
    }

    if(launch_app) {
        /* Enqueue the built-in NFC app to launch after we exit.
         * The NFC app will open with our .nfc file ready to emulate. */
        Loader* loader = furi_record_open(RECORD_LOADER);
        loader_enqueue_launch(loader, "NFC", nfc_path, LoaderDeferredLaunchFlagGui);
        furi_record_close(RECORD_LOADER);

        /* Signal our view dispatcher to stop so the loader can
         * start the NFC app. */
        view_dispatcher_stop(app->view_dispatcher);
    }

    return true;
}
