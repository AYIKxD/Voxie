#include "iot_tools.h"
#include "mcp/mcp_server.h"
#include "system/config.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_random.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"

static const char* TAG = "iot_tools";

static bool s_lamp_state = false;
static led_strip_handle_t s_led_strip = NULL;

void iot_tools_init(void) {
    ESP_LOGI(TAG, "Initializing IoT tools");

    // Init Lamp GPIO
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << LAMP_RELAY_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)LAMP_RELAY_GPIO, 0);
    s_lamp_state = false;

    // Init LED Strip
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_NUM_LEDS,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags = { .with_dma = false },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip));
    led_strip_clear(s_led_strip);
}

static McpResult handle_lamp_turn_on(const cJSON* params) {
    gpio_set_level((gpio_num_t)LAMP_RELAY_GPIO, 1);
    s_lamp_state = true;
    ESP_LOGI(TAG, "Lamp turned ON");
    McpResult res;
    res.success = true;
    res.output = strdup("Lamp is now ON");
    return res;
}

static McpResult handle_lamp_turn_off(const cJSON* params) {
    gpio_set_level((gpio_num_t)LAMP_RELAY_GPIO, 0);
    s_lamp_state = false;
    ESP_LOGI(TAG, "Lamp turned OFF");
    McpResult res;
    res.success = true;
    res.output = strdup("Lamp is now OFF");
    return res;
}

static McpResult handle_lamp_get_state(const cJSON* params) {
    McpResult res;
    res.success = true;
    if (s_lamp_state) {
        res.output = strdup("Lamp is ON");
    } else {
        res.output = strdup("Lamp is OFF");
    }
    return res;
}

static McpResult handle_sensor_read_temperature(const cJSON* params) {
    // TODO: Wire real driver
    float temp = 25.0f + ((float)(esp_random() % 100) / 10.0f - 5.0f); // 20.0 to 30.0
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", temp);
    McpResult res;
    res.success = true;
    res.output = strdup(buf);
    return res;
}

static McpResult handle_sensor_read_humidity(const cJSON* params) {
    // TODO: Wire real driver
    float hum = 60.0f + ((float)(esp_random() % 200) / 10.0f - 10.0f); // 50.0 to 70.0
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", hum);
    McpResult res;
    res.success = true;
    res.output = strdup(buf);
    return res;
}

static McpResult handle_led_strip_set_color(const cJSON* params) {
    McpResult res;
    cJSON* r = cJSON_GetObjectItem(params, "r");
    cJSON* g = cJSON_GetObjectItem(params, "g");
    cJSON* b = cJSON_GetObjectItem(params, "b");

    if (!cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b)) {
        res.success = false;
        res.output = strdup("Invalid or missing RGB parameters");
        return res;
    }

    uint8_t red = (uint8_t)r->valueint;
    uint8_t green = (uint8_t)g->valueint;
    uint8_t blue = (uint8_t)b->valueint;

    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        led_strip_set_pixel(s_led_strip, i, red, green, blue);
    }
    led_strip_refresh(s_led_strip);
    ESP_LOGI(TAG, "LED set color: R:%d G:%d B:%d", red, green, blue);
    
    res.success = true;
    res.output = strdup("LED color set");
    return res;
}

static McpResult handle_led_strip_set_brightness(const cJSON* params) {
    McpResult res;
    cJSON* brightness = cJSON_GetObjectItem(params, "brightness");
    if (!cJSON_IsNumber(brightness)) {
         res.success = false;
         res.output = strdup("Missing brightness parameter");
         return res;
    }
    ESP_LOGI(TAG, "LED set brightness: %d", brightness->valueint);
    res.success = true;
    res.output = strdup("LED brightness set");
    return res;
}

static McpResult handle_led_strip_set_effect(const cJSON* params) {
    McpResult res;
    cJSON* effect = cJSON_GetObjectItem(params, "effect");
    if (!cJSON_IsString(effect)) {
        res.success = false;
        res.output = strdup("Missing effect parameter");
        return res;
    }
    ESP_LOGI(TAG, "LED set effect: %s", effect->valuestring);
    
    if (strcmp(effect->valuestring, "off") == 0) {
        led_strip_clear(s_led_strip);
    }
    
    res.success = true;
    res.output = strdup("LED effect set");
    return res;
}

void iot_tools_register(void) {
    // Lamp tools
    mcp_server_add_tool("self.lamp.turn_on", "Turn on the lamp", "{}", handle_lamp_turn_on);
    mcp_server_add_tool("self.lamp.turn_off", "Turn off the lamp", "{}", handle_lamp_turn_off);
    mcp_server_add_tool("self.lamp.get_state", "Get current lamp state", "{}", handle_lamp_get_state);

    // Sensor tools
    mcp_server_add_tool("self.sensor.read_temperature", "Read temperature", "{}", handle_sensor_read_temperature);
    mcp_server_add_tool("self.sensor.read_humidity", "Read humidity", "{}", handle_sensor_read_humidity);

    // LED tools
    mcp_server_add_tool(
        "self.led_strip.set_color", 
        "Set LED color", 
        "{\"type\":\"object\",\"properties\":{\"r\":{\"type\":\"integer\"},\"g\":{\"type\":\"integer\"},\"b\":{\"type\":\"integer\"}},\"required\":[\"r\",\"g\",\"b\"]}",
        handle_led_strip_set_color
    );
    
    mcp_server_add_tool(
        "self.led_strip.set_brightness",
        "Set LED brightness",
        "{\"type\":\"object\",\"properties\":{\"brightness\":{\"type\":\"integer\"}},\"required\":[\"brightness\"]}",
        handle_led_strip_set_brightness
    );

    mcp_server_add_tool(
        "self.led_strip.set_effect",
        "Set LED effect",
        "{\"type\":\"object\",\"properties\":{\"effect\":{\"type\":\"string\"}},\"required\":[\"effect\"]}",
        handle_led_strip_set_effect
    );
}
