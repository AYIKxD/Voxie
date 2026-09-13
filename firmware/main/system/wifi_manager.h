#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Wi-Fi manager.
 * Checks NVS for credentials. If found, attempts STA connection.
 * If not found or connection fails repeatedly, starts SoftAP provisioning mode.
 */
void wifi_manager_init(void);

/**
 * @brief Check if Wi-Fi is currently connected in STA mode.
 * @return true if connected, false otherwise
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get the current RSSI of the Wi-Fi connection.
 * @return RSSI value, or 0 if not connected
 */
int wifi_manager_get_rssi(void);

/**
 * @brief Get the configured cloud server URL from NVS.
 * @param url_buf Buffer to store the URL
 * @param max_len Maximum length of the buffer
 * @return true if successfully read, false otherwise
 */
bool wifi_manager_get_server_url(char *url_buf, size_t max_len);

#ifdef __cplusplus
}
#endif
