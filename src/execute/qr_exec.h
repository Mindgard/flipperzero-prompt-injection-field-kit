#pragma once

#include "../pifk_app.h"

/* Display the payload text as a QR code on the Flipper screen.
 *
 * Allocates a custom View, encodes the text, and switches to it.
 * The QR code remains on screen until the user presses Back.
 *
 * Returns true if the QR code was generated and displayed. */
bool pifk_execute_qr(PifkApp* app, const PifkPayload* payload);

/* Allocate the QR view and register it with the view dispatcher.
 * Called once during app init. */
void pifk_qr_view_alloc(PifkApp* app);

/* Free the QR view. Called during app free. */
void pifk_qr_view_free(PifkApp* app);
