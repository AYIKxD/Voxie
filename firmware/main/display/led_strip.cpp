#include "led_strip.h"
#include "system/config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "LEDStrip";
static device_state_t s_current_state = DEVICE_STATE_IDLE_LISTENING;

static void led_task(void *arg) {
    while (1) {
        switch (s_current_state) {
            // Mapping these to expected names from the prompt
            // Assuming STATE_IDLE, STATE_STREAMING, STATE_WAITING, STATE_PLAYING, STATE_ERROR
            // are defined in state_machine.h
            // Slow blue breathe
            // Green chase
            // Yellow pulse
            // White gentle breathe
            // Red flash
            default:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void led_strip_init(void) {
    ESP_LOGI(TAG, "Initializing LED strip on GPIO %d", LED_STRIP_GPIO);
    xTaskCreatePinnedToCore(led_task, "led_task", 4096, NULL, 1, NULL, 1);
}

void led_strip_set_state(device_state_t state) {
    s_current_state = state;
    ESP_LOGI(TAG, "LED state changed to %d", (int)state);
}

void led_strip_set_color(uint8_t r, uint8_t g, uint8_t b) {
    ESP_LOGI(TAG, "Setting static color: %d, %d, %d", r, g, b);
}

void led_strip_set_effect(const char *effect) {
    ESP_LOGI(TAG, "Setting effect: %s", effect);
}
