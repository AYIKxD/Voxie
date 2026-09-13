/**
 * Voxie — app_main entry point
 *
 * Initializes all subsystems in dependency order and launches FreeRTOS tasks.
 * The actual work happens in the tasks; this function just wires everything up.
 */
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "system/config.h"
#include "system/state_machine.h"

static const char *TAG = "voxie";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== Voxie Voice Assistant v0.1.0 ===");
    ESP_LOGI(TAG, "Wake word: %s", VOXIE_WAKE_WORD);

    // --- 1. Initialize NVS (needed for Wi-Fi credentials, config) ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // --- 2. Initialize TCP/IP and event loop ---
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // --- 3. Initialize state machine ---
    state_machine_init();

    // --- 4. Initialize audio subsystem (I2S mic + speaker) ---
    // TODO: audio_service_init();
    ESP_LOGI(TAG, "[TODO] Audio service init");

    // --- 5. Initialize KWS engine ---
    // TODO: kws_engine_init();
    ESP_LOGI(TAG, "[TODO] KWS engine init");

    // --- 6. Initialize Wi-Fi (or start SoftAP provisioning) ---
    // TODO: wifi_manager_init();
    ESP_LOGI(TAG, "[TODO] Wi-Fi manager init");

    // --- 7. Initialize display ---
    // TODO: display_manager_init();
    ESP_LOGI(TAG, "[TODO] Display init");

    // --- 8. Initialize protocol (WebSocket client) ---
    // TODO: protocol_init();
    ESP_LOGI(TAG, "[TODO] Protocol init");

#if VOXIE_ENABLE_MCP
    // --- 9. Initialize MCP server ---
    // TODO: mcp_server_init();
    ESP_LOGI(TAG, "[TODO] MCP server init");
#endif

#if VOXIE_ENABLE_IOT
    // --- 10. Register IoT tools ---
    // TODO: iot_tools_init();
    ESP_LOGI(TAG, "[TODO] IoT tools init");
#endif

    ESP_LOGI(TAG, "All subsystems initialized. Entering main loop.");

    // Main task can sleep — all work happens in FreeRTOS tasks
    // We keep it alive for watchdog and heap monitoring
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "State: %s | Free heap: %lu bytes",
                 device_state_to_str(state_machine_get_state()),
                 (unsigned long)esp_get_free_heap_size());
    }
}
