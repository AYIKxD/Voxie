#pragma once

#include "device_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/// Event bits for inter-task signaling
#define EVT_WAKE_WORD_DETECTED   BIT0
#define EVT_STREAM_END           BIT1
#define EVT_TTS_START            BIT2
#define EVT_TTS_END              BIT3
#define EVT_ABORT                BIT4
#define EVT_WIFI_CONNECTED       BIT5
#define EVT_NOTIFY               BIT6
#define EVT_OTA_START            BIT7

/// Initialize the state machine (call once from app_main)
void state_machine_init(void);

/// Get current device state (thread-safe)
device_state_t state_machine_get_state(void);

/// Request a state transition. Returns true if transition is valid.
bool state_machine_transition(device_state_t new_state);

/// Get the shared event group handle for inter-task signaling
EventGroupHandle_t state_machine_get_events(void);
