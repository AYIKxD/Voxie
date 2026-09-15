/**
 * Voxie — app_main entry point
 *
 * Initializes all subsystems in dependency order and launches FreeRTOS tasks.
 * The actual work happens in the tasks; this function just wires everything up.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "cJSON.h"

#include "system/config.h"
#include "system/state_machine.h"
#include "system/wifi_manager.h"
#include "system/ota_manager.h"
#include "audio/audio_service.h"
#include "audio/stream_service.h"
#if VOXIE_HAS_AFE
#include "audio/afe_service.h"
#endif
#include "kws/kws_engine.h"
#include "protocol/websocket_protocol.h"
#include "display/display_manager.h"
#include "display/voxie_led.h"
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

/// Queue a short sine tone on the speaker as audible feedback.
static void play_tone(int freq_hz, int ms) {
    const int n = SPK_SAMPLE_RATE * ms / 1000;
    int16_t *buf = (int16_t *)malloc(n * sizeof(int16_t));
    if (!buf) return;
    for (int i = 0; i < n; i++) {
        buf[i] = (int16_t)(8000.0f *
            sinf(2.0f * 3.14159265f * freq_hz * i / SPK_SAMPLE_RATE));
    }
    audio_service_write_playback(buf, n, 200);
    free(buf);
}

// ============================================================================
// WebSocket message dispatch
// ============================================================================
/// Handle incoming JSON text messages from the cloud server
static void on_ws_text_message(const char *json_str, size_t len) {
    // Parse with cJSON (xiaozhi-style robust dispatch) instead of strstr
    // scanning, which breaks on servers that emit "key": "value" spacing.
    cJSON *root = cJSON_ParseWithLength(json_str, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Invalid JSON from server, ignoring");
        return;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (type == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, "stt") == 0) {
        // Live transcript from ASR — show on display
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        if (text) {
            display_manager_show_transcript(text, true);  // user speech
        }
    }
    else if (strcmp(type, "tts") == 0) {
        // TTS lifecycle
        const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(root, "state"));
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        if (state && strcmp(state, "start") == 0) {
            state_machine_transition(DEVICE_STATE_PLAYING_REPLY);
            if (text) {
                display_manager_show_transcript(text, false);  // assistant reply
            }
        } else if (state && strcmp(state, "end") == 0) {
            state_machine_transition(DEVICE_STATE_IDLE_LISTENING);
        }
    }
    else if (strcmp(type, "llm") == 0) {
        // Emotion updates for display animations
        // TODO: parse emotion and update display face
    }
#if VOXIE_ENABLE_MCP
    else if (strcmp(type, "mcp") == 0) {
        // MCP request from server → device
        if (cJSON_GetObjectItem(root, "method") != NULL) {
            // This is an MCP request (tools/call, etc.)
            std::string response = mcp_server_handle_request(std::string(json_str, len));
            ws_protocol_send_text(response.c_str());
        }
        // MCP responses from device are sent proactively, not handled here
    }
#endif
    else if (strcmp(type, "notify") == 0) {
        // Async notification from cloud: "start" shows the text and enters
        // NOTIFYING (spoken audio follows as TTS binary frames); "end" returns
        // to idle.
        const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(root, "state"));
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        if (state && strcmp(state, "end") == 0) {
            notify_player_end();
        } else {
            notify_player_start(text ? text : "");
        }
    }
    else if (strcmp(type, "hello") == 0) {
        // Server hello response — connection established
        ESP_LOGI(TAG, "Server hello received, session established");
    }
    else if (strcmp(type, "wake") == 0) {
        // Remote debug/test trigger: behave as if a wake word fired.
        ESP_LOGI(TAG, "Remote wake trigger received");
        xEventGroupSetBits(state_machine_get_events(), EVT_WAKE_WORD_DETECTED);
    }

    cJSON_Delete(root);
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
                if (!ws_protocol_is_connected()) {
                    // Without a cloud connection there is nothing to stream to.
                    // Surface the failure (tone + status) instead of silently
                    // entering STREAMING and then doing nothing.
                    ESP_LOGW(TAG, "Wake word detected but server is not connected");
                    display_manager_show_status("Server offline");
                    play_tone(300, 250);
                    continue;  // stays idle
                }

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

        if (bits & EVT_ABORT) {
            // Server dropped or explicitly aborted the session. Without this
            // the device can be stranded in STREAMING / WAITING_REPLY /
            // PLAYING_REPLY forever (xiaozhi recovers to idle in
            // OnAudioChannelClosed). Flush any in-flight audio and return
            // to idle if currently in an active conversational state.
            device_state_t current = state_machine_get_state();
            if (current == DEVICE_STATE_STREAMING ||
                current == DEVICE_STATE_WAITING_REPLY ||
                current == DEVICE_STATE_PLAYING_REPLY ||
                current == DEVICE_STATE_NOTIFYING) {
                ESP_LOGW(TAG, "Abort/disconnect during %s -> returning to idle",
                         device_state_to_str(current));
                stream_service_flush_tts();
                audio_service_flush_playback();
                state_machine_transition(DEVICE_STATE_IDLE_LISTENING);
                display_manager_set_state(DEVICE_STATE_IDLE_LISTENING);
                led_strip_set_state(DEVICE_STATE_IDLE_LISTENING);
            }
        }

        // Update display with current state periodically
        display_manager_set_state(state_machine_get_state());
        led_strip_set_state(state_machine_get_state());
    }
}

// ============================================================================
// Manual trigger — BOOT button (GPIO0, active low) acts as a wake-word event.
// Useful for testing the pipeline and as an accessibility/quiet-demo trigger.
// ============================================================================

static void button_task(void *arg) {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << BUTTON_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    int last = 1;
    while (1) {
        int level = gpio_get_level((gpio_num_t)BUTTON_GPIO);
        if (last == 1 && level == 0) {
            ESP_LOGI(TAG, "Button pressed -> triggering wake");
            device_state_t st = state_machine_get_state();
            if (st == DEVICE_STATE_IDLE_LISTENING || st == DEVICE_STATE_PLAYING_REPLY) {
                xEventGroupSetBits(state_machine_get_events(), EVT_WAKE_WORD_DETECTED);
            }
        }
        last = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================================================
// Re-provisioning — hold BOOT (GPIO0) for 5 s at startup to erase the saved
// Wi-Fi/server settings and reboot into the SoftAP setup portal.
// Ported from xiaozhi's SystemReset (factory reset).
// ============================================================================
#define WIFI_NVS_NAMESPACE "voxie_wifi"
#define RESET_HOLD_MS      5000

static void check_factory_reset_button(void) {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << BUTTON_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    if (gpio_get_level((gpio_num_t)BUTTON_GPIO) != 0) {
        return;  // button not held
    }

    ESP_LOGW(TAG, "BOOT held: keep holding %d s to reset Wi-Fi/server settings...",
             RESET_HOLD_MS / 1000);
    for (int i = 0; i < RESET_HOLD_MS / 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level((gpio_num_t)BUTTON_GPIO) != 0) {
            ESP_LOGI(TAG, "Button released early; normal boot");
            return;
        }
    }

    ESP_LOGW(TAG, "Resetting Wi-Fi/server configuration");
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGW(TAG, "Rebooting into Wi-Fi setup mode...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
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

    // --- 1b. Factory-reset gesture: hold BOOT for 5 s to clear Wi-Fi/server
    // config and reboot into the SoftAP setup portal. Must run before any
    // subsystem reads the saved credentials.
    check_factory_reset_button();

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

    // Startup test tone (400 ms, 660 Hz) to verify the speaker/I2S path.
    {
        const int n = MIC_SAMPLE_RATE * 400 / 1000;
        int16_t *tone = (int16_t *)malloc(n * sizeof(int16_t));
        if (tone) {
            for (int i = 0; i < n; i++) {
                tone[i] = (int16_t)(9000.0f *
                    sinf(2.0f * 3.14159265f * 660.0f * i / MIC_SAMPLE_RATE));
            }
            audio_service_write_playback(tone, n, 1000);
            free(tone);
            ESP_LOGI(TAG, "Startup test tone queued");
        }
    }

#if VOXIE_HAS_AFE
    afe_service_init();
    afe_service_start();
#endif

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

    // --- 14. Start main event loop + manual trigger tasks ---
    xTaskCreatePinnedToCore(main_event_task, "main_evt", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 4, NULL, 1);

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
