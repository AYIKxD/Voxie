#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * WebSocket Protocol — manages connection to cloud backend.
 *
 * Handles:
 *   - Persistent WebSocket connection with auto-reconnect
 *   - JSON text frames for control messages (hello, stream_start/end, stt, tts, mcp, etc.)
 *   - Binary frames for Opus audio with packed headers
 *   - Dispatches incoming messages to appropriate handlers (MCP, TTS, display)
 */

/// Callback types for incoming messages
typedef void (*ws_text_cb_t)(const char *json_str, size_t len);
typedef void (*ws_binary_cb_t)(const uint8_t *data, size_t len);

/// Initialize the WebSocket protocol module
void ws_protocol_init(void);

/// Connect to the cloud server. URL comes from wifi_manager or config.
void ws_protocol_connect(const char *server_url);

/// Disconnect from server
void ws_protocol_disconnect(void);

/// Check if currently connected
bool ws_protocol_is_connected(void);

/// Send a JSON text frame (control message)
bool ws_protocol_send_text(const char *json_str);

/// Send a binary frame (Opus audio with header)
bool ws_protocol_send_binary(const uint8_t *data, size_t len);

/// Send the "hello" handshake message
void ws_protocol_send_hello(const char *device_id, const char *firmware_version);

/// Send "stream_start" with wake timestamp
void ws_protocol_send_stream_start(uint64_t wake_ts_us);

/// Send "stream_end"
void ws_protocol_send_stream_end(void);

/// Send "abort" (barge-in)
void ws_protocol_send_abort(void);

/// Send MCP response
void ws_protocol_send_mcp_result(int id, const char *result_json);

/// Register callback for incoming text messages (JSON)
void ws_protocol_set_text_callback(ws_text_cb_t cb);

/// Register callback for incoming binary messages (Opus TTS audio)
void ws_protocol_set_binary_callback(ws_binary_cb_t cb);
