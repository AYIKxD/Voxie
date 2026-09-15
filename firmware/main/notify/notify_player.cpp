#include "notify_player.h"
#include "audio/audio_service.h"
#include "display/display_manager.h"
#include "system/config.h"
#include "system/state_machine.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "notify";

static volatile bool s_active = false;
static char s_current_text[256] = {0};

void notify_player_init(void) {
    s_active = false;
    s_current_text[0] = '\0';
    ESP_LOGI(TAG, "Notification player initialized");
}

void notify_player_start(const char *text) {
    if (s_active) {
        ESP_LOGW(TAG, "Notification already active, ignoring");
        return;
    }

    // Only play if device is idle (matches xiaozhi: notifications are
    // unsolicited and never interrupt an ongoing conversation).
    device_state_t state = state_machine_get_state();
    if (state != DEVICE_STATE_IDLE_LISTENING) {
        ESP_LOGW(TAG, "Device not idle (state=%s), ignoring notification",
                 device_state_to_str(state));
        return;
    }

    strncpy(s_current_text, text ? text : "", sizeof(s_current_text) - 1);
    s_active = true;

    state_machine_transition(DEVICE_STATE_NOTIFYING);
    display_manager_set_state(DEVICE_STATE_NOTIFYING);
    display_manager_show_status(s_current_text);
    // The audio that follows arrives as Opus binary frames and is decoded
    // and played by stream_service, so no separate download/playback path
    // is needed here.
    ESP_LOGI(TAG, "Notification: %s", s_current_text);
}

void notify_player_end(void) {
    if (!s_active) {
        return;
    }
    s_active = false;
    state_machine_transition(DEVICE_STATE_IDLE_LISTENING);
    display_manager_set_state(DEVICE_STATE_IDLE_LISTENING);
    ESP_LOGI(TAG, "Notification complete");
}

void notify_player_cancel(void) {
    if (!s_active) {
        return;
    }
    ESP_LOGI(TAG, "Cancelling notification");
    audio_service_flush_playback();
    s_active = false;
    state_machine_transition(DEVICE_STATE_IDLE_LISTENING);
    display_manager_set_state(DEVICE_STATE_IDLE_LISTENING);
}

bool notify_player_is_playing(void) {
    return s_active;
}
