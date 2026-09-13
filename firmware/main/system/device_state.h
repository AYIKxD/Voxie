#pragma once

/// Device state machine — all possible runtime states.
/// Transitions are enforced in state_machine.cpp.

typedef enum {
    DEVICE_STATE_UNPROVISIONED,    // No Wi-Fi credentials; running SoftAP portal
    DEVICE_STATE_CONNECTING,       // Connecting to Wi-Fi
    DEVICE_STATE_IDLE_LISTENING,   // KWS running, waiting for wake word
    DEVICE_STATE_STREAMING,        // Wake word detected; streaming Opus to cloud
    DEVICE_STATE_WAITING_REPLY,    // Audio sent; waiting for LLM + TTS response
    DEVICE_STATE_PLAYING_REPLY,    // Playing TTS audio from cloud
    DEVICE_STATE_NOTIFYING,        // Playing async cloud-pushed notification
    DEVICE_STATE_UPGRADING,        // OTA firmware update in progress
    DEVICE_STATE_ERROR,            // Fatal error state
} device_state_t;

/// String representation for display/logging
const char* device_state_to_str(device_state_t state);
