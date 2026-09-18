#pragma once

#include <stdint.h>

/**
 * Binary frame header for Opus audio packets over WebSocket.
 * Binary protocol definitions for high-performance audio transport.
 *
 * All fields are little-endian (native ESP32 byte order).
 */
typedef struct __attribute__((packed)) {
    uint16_t version;       // Protocol version (1)
    uint16_t type;          // 0 = opus_mic_audio, 1 = opus_tts_audio
    uint32_t timestamp_ms;  // SNTP-synced epoch milliseconds (or device uptime)
    uint32_t payload_size;  // Size of the Opus frame payload following this header
} binary_frame_header_t;

#define BINARY_PROTO_VERSION        1
#define BINARY_TYPE_MIC_AUDIO       0
#define BINARY_TYPE_TTS_AUDIO       1
#define BINARY_HEADER_SIZE          sizeof(binary_frame_header_t)  // 12 bytes
