#pragma once

/**
 * OTA Manager — dual-partition firmware updates via MCP tool.
 *
 * Supports:
 *   - Download firmware from HTTP/HTTPS URL to inactive OTA partition
 *   - Image validation before marking bootable
 *   - Automatic rollback on boot failure
 *   - Progress reporting via callback
 */

/// Initialize OTA subsystem (validates current partition, cancels rollback on good boot)
void ota_manager_init(void);

/// Start OTA update from the given URL. Blocks until complete or failure.
/// Returns ESP_OK on success (device will reboot), or error code.
#include "esp_err.h"
esp_err_t ota_start_update(const char *url);
