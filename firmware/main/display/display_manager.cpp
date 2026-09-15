#include "display_manager.h"
#include "system/config.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "DisplayManager";
static bool s_dark_theme = true;
static device_state_t s_last_state = DEVICE_STATE_CONNECTING;
static bool s_state_valid = false;

void display_manager_init(void) {
    ESP_LOGI(TAG, "Initializing display manager");

#if VOXIE_ENABLE_LCD
    ESP_LOGI(TAG, "Configuring LCD (ST7789) on pins MOSI:%d SCLK:%d CS:%d DC:%d RST:%d BL:%d",
             LCD_PIN_MOSI, LCD_PIN_SCLK, LCD_PIN_CS, LCD_PIN_DC, LCD_PIN_RST, LCD_PIN_BL);

    // Initialize backlight PWM
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode      = LEDC_LOW_SPEED_MODE;
    ledc_timer.duty_resolution = LEDC_TIMER_8_BIT;
    ledc_timer.timer_num       = LEDC_TIMER_0;
    ledc_timer.freq_hz         = 4000;
    ledc_timer.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num   = LCD_PIN_BL;
    ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_channel.channel    = LEDC_CHANNEL_0;
    ledc_channel.intr_type  = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel  = LEDC_TIMER_0;
    ledc_channel.duty       = 128;
    ledc_channel.hpoint     = 0;
    ledc_channel_config(&ledc_channel);

#ifdef VOXIE_HAS_LVGL
    ESP_LOGI(TAG, "LVGL enabled. Initializing UI...");
#else
    ESP_LOGW(TAG, "LVGL not available. Using simple text rendering fallback.");
#endif

#else
    ESP_LOGI(TAG, "Configuring OLED (SSD1306) via I2C");
#endif
}

void display_manager_set_state(device_state_t state) {
    // Skip no-op updates. The main loop re-asserts the state every second as
    // a safety net, and without this guard that produced a log line (and a
    // full redraw) per second even when nothing changed.
    if (s_state_valid && state == s_last_state) {
        return;
    }
    s_last_state = state;
    s_state_valid = true;

    ESP_LOGI(TAG, "Display state changed to %d", (int)state);
#if VOXIE_ENABLE_LCD
#ifdef VOXIE_HAS_LVGL
    // Update status bar and show state animation (pulsing dots / mic icon)
#endif
#else
    // Show state on OLED line 1
#endif
}

void display_manager_show_transcript(const char *text, bool is_user) {
    ESP_LOGI(TAG, "Transcript (%s): %s", is_user ? "User" : "Assistant", text);
#if VOXIE_ENABLE_LCD
#ifdef VOXIE_HAS_LVGL
    // Add chat bubble
#endif
#else
    // Scroll text on OLED lines 2-4
#endif
}

void display_manager_show_status(const char *status) {
    ESP_LOGI(TAG, "Status update: %s", status);
}

void display_manager_set_brightness(int brightness) {
#if VOXIE_ENABLE_LCD
    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;
    uint32_t duty = (brightness * 255) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#endif
}

void display_manager_set_theme(bool dark) {
    s_dark_theme = dark;
    ESP_LOGI(TAG, "Theme set to %s", dark ? "dark" : "light");
}
