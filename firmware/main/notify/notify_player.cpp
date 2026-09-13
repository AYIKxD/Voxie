#include "notify_player.h"
#include "audio/audio_service.h"
#include "system/config.h"
#include "system/state_machine.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "notify";

static volatile bool s_playing = false;
static volatile bool s_cancel_requested = false;
static char s_current_text[256] = {0};
static char s_current_url[512] = {0};

/// Task that downloads and plays notification audio
static void notify_task(void *arg) {
    ESP_LOGI(TAG, "Playing notification: %s", s_current_text);
    ESP_LOGI(TAG, "Audio URL: %s", s_current_url);

    state_machine_transition(DEVICE_STATE_NOTIFYING);

    // TODO: implement full Opus streaming from HTTP URL
    // For now, we just show the text and play a simple beep pattern
    // The full implementation would:
    // 1. HTTP GET the audio_url
    // 2. Stream Ogg Opus data through a demuxer
    // 3. Decode Opus frames to PCM
    // 4. Feed PCM to audio_service_write_playback()

    // Simple notification beep (440 Hz sine, 200ms)
    const int beep_samples = MIC_SAMPLE_RATE / 5;  // 200ms
    int16_t *beep_buf = (int16_t *)malloc(beep_samples * sizeof(int16_t));
    if (beep_buf) {
        for (int i = 0; i < beep_samples; i++) {
            // 440 Hz sine wave
            float t = (float)i / MIC_SAMPLE_RATE;
            beep_buf[i] = (int16_t)(8000.0f * sinf(2.0f * 3.14159f * 440.0f * t));
        }

        if (!s_cancel_requested) {
            audio_service_write_playback(beep_buf, beep_samples, 1000);
        }
        free(beep_buf);
    }

    // Wait for playback to finish (approximate)
    vTaskDelay(pdMS_TO_TICKS(500));

    // Display the notification text
    // TODO: wire to display_manager_show_status(s_current_text)

    s_playing = false;
    s_cancel_requested = false;
    state_machine_transition(DEVICE_STATE_IDLE_LISTENING);

    ESP_LOGI(TAG, "Notification playback complete");
    vTaskDelete(NULL);
}

void notify_player_init(void) {
    s_playing = false;
    s_cancel_requested = false;
    ESP_LOGI(TAG, "Notification player initialized");
}

void notify_player_play(const char *text, const char *audio_url) {
    if (s_playing) {
        ESP_LOGW(TAG, "Already playing a notification, ignoring");
        return;
    }

    // Only play if device is idle
    device_state_t state = state_machine_get_state();
    if (state != DEVICE_STATE_IDLE_LISTENING) {
        ESP_LOGW(TAG, "Device not idle (state=%s), ignoring notification",
                 device_state_to_str(state));
        return;
    }

    strncpy(s_current_text, text ? text : "", sizeof(s_current_text) - 1);
    strncpy(s_current_url, audio_url ? audio_url : "", sizeof(s_current_url) - 1);
    s_playing = true;
    s_cancel_requested = false;

    xTaskCreatePinnedToCore(notify_task, "notify", 4096, NULL, 3, NULL, 1);
}

void notify_player_cancel(void) {
    if (s_playing) {
        ESP_LOGI(TAG, "Cancelling notification");
        s_cancel_requested = true;
        audio_service_flush_playback();
    }
}

bool notify_player_is_playing(void) {
    return s_playing;
}
