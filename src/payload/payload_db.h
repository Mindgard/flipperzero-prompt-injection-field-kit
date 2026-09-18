#pragma once

#include "../pifk_app.h"

/* Write `s` to `file` with JSON string escaping applied.  Shared with
 * the capture log so there is one escaper rather than two: quotes,
 * backslashes, newlines and control bytes below 0x20 are escaped, while
 * bytes >= 0x80 pass through so valid UTF-8 round-trips unchanged. */
void write_json_escaped(File* file, const char* s);

/* Load the compiled-in default payloads into the database. */
void payload_db_load_builtins(PayloadDb* db);

/* Load additional payloads from a JSON file on the SD card.
 * Payloads with names matching existing entries are skipped. */
void payload_db_load_from_file(PayloadDb* db, const char* path);

/* Export the user-authored payloads to SD card JSON, creating a
 * human-editable file.  Builtins are excluded: they already live in flash
 * and are rejected by name on load, so writing them would only inflate
 * the file this app must read back into heap at every launch.  With no
 * user payloads to write, a short schema stub is emitted instead. */
bool payload_db_export_user(const PayloadDb* db, const char* path);

/* Find a payload by name. Returns NULL if not found. */
const PifkPayload* payload_db_find(const PayloadDb* db, const char* name);

/* Toggle favorite status for payload at index. */
void payload_db_toggle_favorite(PayloadDb* db, uint16_t index);

/* Save favorite payload names to a JSON file on the SD card. */
bool favorites_save(const PayloadDb* db, const char* path);

/* Load favorites from a JSON file and mark matching payloads. */
void favorites_load(PayloadDb* db, const char* path);

/* Reload all data from SD card (payloads, favorites, settings). */
void pifk_reload_data(PifkApp* app);

/* Save app settings (BadUSB delay, GPIO/UART, I2C) to SD card. */
bool settings_save(const PifkApp* app, const char* path);

/* Load app settings from SD card. */
void settings_load(PifkApp* app, const char* path);

/* Free heap-allocated text in payload entries (owned strings). */
void payload_db_free_entries(PayloadDb* db);
