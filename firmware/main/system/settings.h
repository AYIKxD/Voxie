#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Settings — lightweight NVS-backed key/value store.
 * Defines the core Settings class.
 *
 * Namespaces isolate subsystems (e.g. "voxie_audio") so keys stay short
 * and collisions between subsystems are impossible.
 *
 * Keys are persistent API: once shipped, renaming requires migration.
 */

/// Open a namespace. read_write=false opens read-only.
/// Returns true on success; false if the handle could not be opened
/// (e.g. NVS not initialized or namespace creation failed).
bool settings_open(const char *ns, bool read_write);

/// Close the currently open namespace (commits if opened read-write).
void settings_close(void);

/// String values. Returns default_value when the key is absent.
/// Returns the number of bytes written to out_buf (excluding NUL).
size_t settings_get_string(const char *key, char *out_buf, size_t buf_len,
                           const char *default_value);

/// Store a string value.
bool settings_set_string(const char *key, const char *value);

/// Signed 32-bit integer values.
int32_t settings_get_int(const char *key, int32_t default_value);
bool settings_set_int(const char *key, int32_t value);

/// Boolean values (stored as u8).
bool settings_get_bool(const char *key, bool default_value);
bool settings_set_bool(const char *key, bool value);

/// Remove a single key from the open namespace.
bool settings_erase_key(const char *key);

/// Erase every key in the open namespace.
bool settings_erase_all(void);

#ifdef __cplusplus
}
#endif
