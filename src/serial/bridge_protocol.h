#pragma once

#include "../pifk_app.h"

/* Start the serial bridge listener thread.
 * Listens on USB CDC virtual COM port for commands from Prompt
 * Injection Studio. */
void bridge_start(PifkApp* app);

/* Stop the serial bridge listener thread and clean up. */
void bridge_stop(PifkApp* app);

/* Send a response line back to the host via serial. */
void bridge_send(PifkApp* app, const char* response);

/* Process a single received command line from the host.
 * Called by the bridge thread when a complete line is received. */
void bridge_handle_command(PifkApp* app, const char* cmd);

/* Thread flags for bridge control. */
#define BRIDGE_THREAD_FLAG_STOP (1 << 0)
