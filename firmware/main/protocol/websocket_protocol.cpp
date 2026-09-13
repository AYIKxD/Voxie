#include "websocket_protocol.h"
#include "binary_protocol.h"
#include "system/config.h"
#include "system/state_machine.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ws_proto";

static esp_websocket_client_handle_t s_ws_client = NULL;
static ws_text_cb_t s_text_cb = NULL;
static ws_binary_cb_t s_binary_cb = NULL;
static bool s_connected = false;

// ============================================================================
// WebSocket event handler
// ============================================================================

static void ws_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            s_connected = true;
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            s_connected = false;
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01) {
                // Text frame (JSON control message)
                if (s_text_cb && data->data_ptr && data->data_len > 0) {
                    // Ensure null-terminated string
                    char *json_buf = (char *)malloc(data->data_len + 1);
                    if (json_buf) {
                        memcpy(json_buf, data->data_ptr, data->data_len);
                        json_buf[data->data_len] = '\0';
                        s_text_cb(json_buf, data->data_len);
                        free(json_buf);
                    }
                }
            } else if (data->op_code == 0x02) {
                // Binary frame (Opus audio)
                if (s_binary_cb && data->data_ptr && data->data_len > 0) {
                    s_binary_cb((const uint8_t *)data->data_ptr, data->data_len);
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;

        default:
            break;
    }
}

// ============================================================================
// Public API
// ============================================================================

void ws_protocol_init(void) {
    ESP_LOGI(TAG, "WebSocket protocol module initialized");
}

void ws_protocol_connect(const char *server_url) {
    if (s_ws_client != NULL) {
        ws_protocol_disconnect();
    }

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = server_url;
    ws_cfg.reconnect_timeout_ms = WS_RECONNECT_INTERVAL_MS;
    ws_cfg.network_timeout_ms = 10000;
    ws_cfg.buffer_size = 4096;

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (s_ws_client == NULL) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return;
    }

    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                   ws_event_handler, NULL);

    esp_err_t ret = esp_websocket_client_start(s_ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Connecting to %s", server_url);
    }
}

void ws_protocol_disconnect(void) {
    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        s_connected = false;
        ESP_LOGI(TAG, "WebSocket disconnected");
    }
}

bool ws_protocol_is_connected(void) {
    return s_connected;
}

bool ws_protocol_send_text(const char *json_str) {
    if (!s_connected || !s_ws_client) return false;
    int ret = esp_websocket_client_send_text(s_ws_client, json_str, strlen(json_str),
                                              pdMS_TO_TICKS(1000));
    return (ret > 0);
}

bool ws_protocol_send_binary(const uint8_t *data, size_t len) {
    if (!s_connected || !s_ws_client) return false;
    int ret = esp_websocket_client_send_bin(s_ws_client, (const char *)data, len,
                                             pdMS_TO_TICKS(1000));
    return (ret > 0);
}

void ws_protocol_send_hello(const char *device_id, const char *firmware_version) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"hello\","
        "\"device_id\":\"%s\","
        "\"firmware_version\":\"%s\","
        "\"features\":{\"mcp\":%s,\"opus\":true,\"vision\":%s},"
        "\"sample_rate\":%d,"
        "\"opus_frame_ms\":%d}",
        device_id,
        firmware_version,
        VOXIE_ENABLE_MCP ? "true" : "false",
        VOXIE_ENABLE_CAMERA ? "true" : "false",
        MIC_SAMPLE_RATE,
        OPUS_FRAME_DURATION_MS
    );
    ws_protocol_send_text(buf);
    ESP_LOGI(TAG, "Sent hello: %s", buf);
}

void ws_protocol_send_stream_start(uint64_t wake_ts_us) {
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"stream_start\",\"wake_ts_us\":%llu,\"mode\":\"auto\"}",
        (unsigned long long)wake_ts_us);
    ws_protocol_send_text(buf);
    ESP_LOGI(TAG, "Sent stream_start (wake_ts=%llu us)", (unsigned long long)wake_ts_us);
}

void ws_protocol_send_stream_end(void) {
    ws_protocol_send_text("{\"type\":\"stream_end\"}");
    ESP_LOGI(TAG, "Sent stream_end");
}

void ws_protocol_send_abort(void) {
    ws_protocol_send_text("{\"type\":\"abort\",\"reason\":\"wake_word_detected\"}");
    ESP_LOGI(TAG, "Sent abort (barge-in)");
}

void ws_protocol_send_mcp_result(int id, const char *result_json) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"mcp\",\"id\":%d,\"result\":%s}", id, result_json);
    ws_protocol_send_text(buf);
}

void ws_protocol_set_text_callback(ws_text_cb_t cb) {
    s_text_cb = cb;
}

void ws_protocol_set_binary_callback(ws_binary_cb_t cb) {
    s_binary_cb = cb;
}
