/**
 * Voxie — app_main entry point
 *
 * Initializes all subsystems in dependency order and launches FreeRTOS tasks.
 * The actual work happens in the tasks; this function just wires everything up.
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"

#include "system/config.h"
#include "system/state_machine.h"
#include "system/wifi_manager.h"
#include "system/ota_manager.h"
#include "audio/audio_service.h"
#include "audio/stream_service.h"
#include "kws/kws_engine.h"
#include "protocol/websocket_protocol.h"
#include "display/display_manager.h"
#include "display/led_strip.h"
#include "notify/notify_player.h"

#if VOXIE_ENABLE_MCP
#include "mcp/mcp_server.h"
#include "mcp/builtin_tools.h"
#endif

#if VOXIE_ENABLE_IOT
#include "iot/iot_tools.h"
#endif

static const char *TAG = "voxie";

#define FIRMWARE_VERSION "0.1.0"

// ============================================================================
// WebSocket message dispatch
// ============================================================================

/// Handle incoming JSON text messages from the cloud server
static void on_ws_text_message(const char *json_str, size_t len) {
    // Simple type dispatch — extract "type" field
    const char *type_pos = strstr(json_str, "\"type\":");
    if (!type_pos) return;

    if (strstr(type_pos, "\"stt\"")) {
        // Live transcript from ASR — show on display
        const char *text_pos = strstr(json_str, "\"text\":\"");
        if (text_pos) {
            text_pos += 8;
            char text[256];
            const char *end = strchr(text_pos, '"');
            if (end) {
                size_t tlen = end - text_pos;
                if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
                memcpy(text, text_pos, tlen);
                text[tlen] = '\0';
                display_manager_show_transcript(text, true);  // user speech
            }
        }
    }
    else if (strstr(type_pos, "\"tts\"")) {
        // TTS lifecycle
        if (strstr(json_str, "\"start\"")) {
            state_machine_transition(DEVICE_STATE_PLAYING_REPLY);
            // Extract reply text for display
            const char *text_pos = strstr(json_str, "\"text\":\"");
            if (text_pos) {
                text_pos += 8;
                char text[512];
                const char *end = strchr(text_pos, '"');
                if (end) {
                    size_t tlen = end - text_pos;
                    if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
                    memcpy(text, text_pos, tlen);
                    text[tlen] = '\0';
                    display_manager_show_transcript(text, false);  // assistant reply
                }
            }
        } else if (strstr(json_str, "\"end\"")) {
            state_machine_transition(DEVICE_STATE_IDLE_LISTENING);
        }
    }
    else if (strstr(type_pos, "\"llm\"")) {
        // Emotion updates for display animations
        // TODO: parse emotion and update display face
    }
#if VOXIE_ENABLE_MCP
    else if (strstr(type_pos, "\"mcp\"")) {
        // MCP request from server → device
        if (strstr(json_str, "\"method\"")) {
            // This is an MCP request (tools/call, etc.)
            std::string response = mcp_server_handle_request(std::string(json_str, len));
            ws_protocol_send_text(response.c_str());
        }
        // MCP responses from device are sent proactively, not handled here
    }
#endif
    else if (strstr(type_pos, "\"notify\"")) {
        // Async notification from cloud
        const char *text_pos = strstr(json_str, "\"text\":\"");
        const char *url_pos = strstr(json_str, "\"audio_url\":\"");
        char text[256] = "";
        char url[512] = "";

        if (text_pos) {
            text_pos += 8;
            const char *end = strchr(text_pos, '"');
            if (end) {
                size_t tlen = end - text_pos;
                if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
                memcpy(text, text_pos, tlen);
                text[tlen] = '\0';
            }
        }
        if (url_pos) {
            url_pos += 13;
            const char *end = strchr(url_pos, '"');
            if (end) {
                size_t ulen = end - url_pos;
                if (ulen >= sizeof(url)) ulen = sizeof(url) - 1;
                memcpy(url, url_pos, ulen);
                url[ulen] = '\0';
            }
        }
        notify_player_play(text, url);
    }
    else if (strstr(type_pos, "\"hello\"")) {
        // Server hello response — connection established
        ESP_LOGI(TAG, "Server hello received, session established");
    }
}

/// Handle incoming binary messages (Opus TTS audio from cloud)
static void on_ws_binary_message(const uint8_t *data, size_t len) {
    // Binary frames are header + Opus-encoded TTS audio from the cloud.
    stream_service_handle_tts(data, len);
}

// ============================================================================
// Main event loop — handles wake word detection and streaming
// ============================================================================

static void main_event_task(void *arg) {
    EventGroupHandle_t events = state_machine_get_events();

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(events,
            EVT_WAKE_WORD_DETECTED | EVT_STREAM_END | EVT_ABORT,
            pdTRUE,  // clear bits on return
            pdFALSE, // wait for ANY bit
            pdMS_TO_TICKS(1000));

        if (bits & EVT_WAKE_WORD_DETECTED) {
            device_state_t current = state_machine_get_state();

            if (current == DEVICE_STATE_PLAYING_REPLY) {
                // Barge-in: stop current playback, start new stream
                ESP_LOGI(TAG, "Barge-in detected!");
                stream_service_flush_tts();
                audio_service_flush_playback();
                ws_protocol_send_abort();
            }

            if (current == DEVICE_STATE_IDLE_LISTENING ||
                current == DEVICE_STATE_PLAYING_REPLY) {
                state_machine_transition(DEVICE_STATE_STREAMING);
                display_manager_set_state(DEVICE_STATE_STREAMING);
                led_strip_set_state(DEVICE_STATE_STREAMING);

                // Get SNTP-synced timestamp for latency measurement
                struct timeval tv;
                gettimeofday(&tv, NULL);
                uint64_t wake_ts_us = (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;

                ws_protocol_send_stream_start(wake_ts_us);
                stream_service_start();
                ESP_LOGI(TAG, "Streaming started (wake_ts=%llu us)",
                         (unsigned long long)wake_ts_us);
            }
        }

        if (bits & EVT_STREAM_END) {
            state_machine_transition(DEVICE_STATE_WAITING_REPLY);
            display_manager_set_state(DEVICE_STATE_WAITING_REPLY);
            led_strip_set_state(DEVICE_STATE_WAITING_REPLY);
            ws_protocol_send_stream_end();
        }

        // Update display with current state periodically
        display_manager_set_state(state_machine_get_state());
        led_strip_set_state(state_machine_get_state());
    }
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== Voxie Voice Assistant v%s ===", FIRMWARE_VERSION);
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

    // --- 4. Initialize OTA manager (validate current partition) ---
#if VOXIE_ENABLE_OTA
    ota_manager_init();
#endif

    // --- 5. Initialize audio subsystem (I2S mic + speaker) ---
    audio_service_init();
    audio_service_start();

    // --- 5b. Initialize the Opus streaming service (pre-roll + codec) ---
    stream_service_init();

    // --- 6. Initialize KWS engine (feature extraction + inference) ---
    kws_engine_init();
    kws_engine_start();

    // --- 7. Initialize display + LED strip ---
    display_manager_init();
    led_strip_init();

    // --- 8. Initialize notification player ---
    notify_player_init();

    // --- 9. Initialize protocol layer ---
    ws_protocol_init();
    ws_protocol_set_text_callback(on_ws_text_message);
    ws_protocol_set_binary_callback(on_ws_binary_message);

#if VOXIE_ENABLE_MCP
    // --- 10. Initialize MCP server + register built-in tools ---
    mcp_server_init();
    builtin_tools_register();
#endif

#if VOXIE_ENABLE_IOT
    // --- 11. Initialize and register IoT tools ---
    iot_tools_init();
    iot_tools_register();
#endif

    // --- 12. Initialize Wi-Fi (or start SoftAP provisioning) ---
    // This is last because it may block waiting for connection
    wifi_manager_init();

    // --- 13. Connect to cloud server once Wi-Fi is up ---
    // Wait for Wi-Fi, logging periodically so the console stays observable.
    EventGroupHandle_t events = state_machine_get_events();
    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    while ((xEventGroupGetBits(events) & EVT_WIFI_CONNECTED) == 0) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        ESP_LOGI(TAG, "Still waiting for Wi-Fi (state=%s)...",
                 device_state_to_str(state_machine_get_state()));
    }

    ESP_LOGI(TAG, "Wi-Fi connected! Connecting to cloud server...");
    state_machine_transition(DEVICE_STATE_IDLE_LISTENING);
    display_manager_set_state(DEVICE_STATE_IDLE_LISTENING);
    led_strip_set_state(DEVICE_STATE_IDLE_LISTENING);

    // Register device identity; the hello handshake is sent automatically when
    // the WebSocket connects (sending it here would race the connection).
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_id[18];
    snprintf(device_id, sizeof(device_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ws_protocol_set_device_info(device_id, FIRMWARE_VERSION);

    // Connect to server
    char server_url[256] = {0};
    if (wifi_manager_get_server_url(server_url, sizeof(server_url)) && strlen(server_url) > 0) {
        ws_protocol_connect(server_url);
    } else {
        ESP_LOGW(TAG, "No server URL configured, using default");
        ws_protocol_connect("ws://192.168.1.100:8000/ws");
    }

    // --- 14. Start main event loop task ---
    xTaskCreatePinnedToCore(main_event_task, "main_evt", 4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "All subsystems initialized. Voxie is ready!");

    // Main task stays alive for heap monitoring
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "State: %s | Free heap: %lu | Min heap: %lu",
                 device_state_to_str(state_machine_get_state()),
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size());
    }
}
