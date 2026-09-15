#include "state_machine.h"
#include "device_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "state_machine";

static device_state_t s_current_state = DEVICE_STATE_CONNECTING;
static EventGroupHandle_t s_events = NULL;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

const char* device_state_to_str(device_state_t state) {
    switch (state) {
        case DEVICE_STATE_UNPROVISIONED:  return "UNPROVISIONED";
        case DEVICE_STATE_CONNECTING:     return "CONNECTING";
        case DEVICE_STATE_IDLE_LISTENING: return "IDLE_LISTENING";
        case DEVICE_STATE_STREAMING:      return "STREAMING";
        case DEVICE_STATE_WAITING_REPLY:  return "WAITING_REPLY";
        case DEVICE_STATE_PLAYING_REPLY:  return "PLAYING_REPLY";
        case DEVICE_STATE_NOTIFYING:      return "NOTIFYING";
        case DEVICE_STATE_UPGRADING:      return "UPGRADING";
        case DEVICE_STATE_ERROR:          return "ERROR";
        default:                          return "UNKNOWN";
    }
}

/// Validate whether a given transition is allowed
static bool is_valid_transition(device_state_t from, device_state_t to) {
    // OTA and ERROR can be entered from any state
    if (to == DEVICE_STATE_UPGRADING || to == DEVICE_STATE_ERROR) {
        return true;
    }

    switch (from) {
        case DEVICE_STATE_UNPROVISIONED:
            return (to == DEVICE_STATE_CONNECTING);

        case DEVICE_STATE_CONNECTING:
            return (to == DEVICE_STATE_IDLE_LISTENING ||
                    to == DEVICE_STATE_UNPROVISIONED);

        case DEVICE_STATE_IDLE_LISTENING:
            return (to == DEVICE_STATE_STREAMING ||
                    to == DEVICE_STATE_NOTIFYING ||
                    to == DEVICE_STATE_PLAYING_REPLY);  // server-initiated TTS

        case DEVICE_STATE_STREAMING:
            return (to == DEVICE_STATE_WAITING_REPLY ||
                    to == DEVICE_STATE_PLAYING_REPLY ||  // reply arrives mid-stream
                    to == DEVICE_STATE_IDLE_LISTENING);  // abort/timeout

        case DEVICE_STATE_WAITING_REPLY:
            return (to == DEVICE_STATE_PLAYING_REPLY ||
                    to == DEVICE_STATE_IDLE_LISTENING);  // timeout/error

        case DEVICE_STATE_PLAYING_REPLY:
            return (to == DEVICE_STATE_IDLE_LISTENING || // TTS done
                    to == DEVICE_STATE_PLAYING_REPLY ||  // next sentence/clip
                    to == DEVICE_STATE_STREAMING);       // barge-in

        case DEVICE_STATE_NOTIFYING:
            return (to == DEVICE_STATE_IDLE_LISTENING);

        case DEVICE_STATE_UPGRADING:
            return (to == DEVICE_STATE_IDLE_LISTENING);  // after reboot

        case DEVICE_STATE_ERROR:
            return (to == DEVICE_STATE_CONNECTING);       // recovery attempt

        default:
            return false;
    }
}

void state_machine_init(void) {
    s_events = xEventGroupCreate();
    configASSERT(s_events != NULL);
    s_current_state = DEVICE_STATE_CONNECTING;
    ESP_LOGI(TAG, "State machine initialized -> %s", device_state_to_str(s_current_state));
}

device_state_t state_machine_get_state(void) {
    device_state_t state;
    portENTER_CRITICAL(&s_lock);
    state = s_current_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

bool state_machine_transition(device_state_t new_state) {
    portENTER_CRITICAL(&s_lock);
    device_state_t old_state = s_current_state;

    if (old_state == new_state) {
        // No-op: repeated notifications (e.g. several TTS "start"/"end"
        // messages) must not spam the log or be treated as errors.
        portEXIT_CRITICAL(&s_lock);
        return true;
    }

    if (!is_valid_transition(old_state, new_state)) {
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "Invalid transition: %s -> %s",
                 device_state_to_str(old_state), device_state_to_str(new_state));
        return false;
    }

    s_current_state = new_state;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "Transition: %s -> %s",
             device_state_to_str(old_state), device_state_to_str(new_state));
    return true;
}

EventGroupHandle_t state_machine_get_events(void) {
    return s_events;
}
