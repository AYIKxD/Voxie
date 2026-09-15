#include "voxie_led.h"
#include "system/config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <led_strip.h>

static led_strip_handle_t s_led_strip;
static const char* TAG = "LEDStrip";
static device_state_t s_current_state = DEVICE_STATE_IDLE_LISTENING;

static void led_task(void *arg) {
    uint32_t step = 0;
    while (1) {
        step++;
        switch (s_current_state) {
            case DEVICE_STATE_IDLE_LISTENING: {
                // slow blue breathe
                uint8_t val = (step % 40) < 20 ? (step % 20) : (19 - (step % 20));
                for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) led_strip_set_pixel(s_led_strip, i, 0, 0, val * 2);
                led_strip_refresh(s_led_strip);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            }
            case DEVICE_STATE_STREAMING: {
                // fast green chase
                led_strip_clear(s_led_strip);
                led_strip_set_pixel(s_led_strip, step % LED_STRIP_NUM_LEDS, 0, 50, 0);
                led_strip_refresh(s_led_strip);
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
            }
            case DEVICE_STATE_WAITING_REPLY: {
                // yellow pulse
                uint8_t val = (step % 20) < 10 ? (step % 10) : (9 - (step % 10));
                for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) led_strip_set_pixel(s_led_strip, i, val * 10, val * 8, 0);
                led_strip_refresh(s_led_strip);
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
            }
            case DEVICE_STATE_PLAYING_REPLY: {
                // white gentle breathe
                uint8_t val = (step % 40) < 20 ? (step % 20) : (19 - (step % 20));
                for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) led_strip_set_pixel(s_led_strip, i, val * 2, val * 2, val * 2);
                led_strip_refresh(s_led_strip);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            }
            default:
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}

void led_strip_init(void) {
    ESP_LOGI(TAG, "Initializing LED strip on GPIO %d", LED_STRIP_GPIO);
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = LED_STRIP_GPIO;
    strip_config.max_leds = LED_STRIP_NUM_LEDS;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    rmt_config.flags.with_dma = false;

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip));
    led_strip_clear(s_led_strip);
    xTaskCreatePinnedToCore(led_task, "led_task", 4096, NULL, 1, NULL, 1);
}

void led_strip_set_state(device_state_t state) {
    // Ignore redundant updates (the main loop re-asserts state every second)
    // so re-setting the same state does not spam the log or restart effects.
    if (state == s_current_state) {
        return;
    }
    s_current_state = state;
    ESP_LOGI(TAG, "LED state changed to %d", (int)state);
}

void led_strip_set_color(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) led_strip_set_pixel(s_led_strip, i, r, g, b);
    led_strip_refresh(s_led_strip);
}

void led_strip_set_effect(const char *effect) {
    ESP_LOGI(TAG, "Setting effect: %s", effect);
}
