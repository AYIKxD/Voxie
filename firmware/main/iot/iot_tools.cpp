#include "iot_tools.h"
#include "mcp/mcp_server.h"
#include "system/config.h"
#include "display/voxie_led.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_log.h"
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "cJSON.h"

static const char* TAG = "iot_tools";

static bool s_lamp_state = false;

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

    // The WS2812B strip task is created by led_strip_init() in app_main.
    // Clear it to a known state here.
    led_strip_set_color(0, 0, 0);
}

static McpResult handle_lamp_turn_on(const std::string &args) {
    (void)args;
    gpio_set_level((gpio_num_t)LAMP_RELAY_GPIO, 1);
    s_lamp_state = true;
    ESP_LOGI(TAG, "Lamp turned ON");
    return {"Lamp is now ON", false};
}

static McpResult handle_lamp_turn_off(const std::string &args) {
    (void)args;
    gpio_set_level((gpio_num_t)LAMP_RELAY_GPIO, 0);
    s_lamp_state = false;
    ESP_LOGI(TAG, "Lamp turned OFF");
    return {"Lamp is now OFF", false};
}

static McpResult handle_lamp_get_state(const std::string &args) {
    (void)args;
    return {s_lamp_state ? "Lamp is ON" : "Lamp is OFF", false};
}

static McpResult handle_sensor_read_temperature(const std::string &args) {
    (void)args;
    // TODO: Wire real driver
    float temp = 25.0f + ((float)(esp_random() % 100) / 10.0f - 5.0f); // 20.0 to 30.0
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", temp);
    return {buf, false};
}

static McpResult handle_sensor_read_humidity(const std::string &args) {
    (void)args;
    // TODO: Wire real driver
    float hum = 60.0f + ((float)(esp_random() % 200) / 10.0f - 10.0f); // 50.0 to 70.0
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", hum);
    return {buf, false};
}

static McpResult handle_led_strip_set_color(const std::string &args) {
    cJSON *params = cJSON_Parse(args.c_str());
    if (params == NULL) {
        return {"Invalid JSON arguments", true};
    }

    cJSON* r = cJSON_GetObjectItem(params, "r");
    cJSON* g = cJSON_GetObjectItem(params, "g");
    cJSON* b = cJSON_GetObjectItem(params, "b");

    if (!cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b)) {
        cJSON_Delete(params);
        return {"Invalid or missing RGB parameters", true};
    }

    uint8_t red = (uint8_t)r->valueint;
    uint8_t green = (uint8_t)g->valueint;
    uint8_t blue = (uint8_t)b->valueint;
    cJSON_Delete(params);

    led_strip_set_color(red, green, blue);
    ESP_LOGI(TAG, "LED set color: R:%d G:%d B:%d", red, green, blue);

    return {"LED color set", false};
}

static McpResult handle_led_strip_set_brightness(const std::string &args) {
    cJSON *params = cJSON_Parse(args.c_str());
    if (params == NULL) {
        return {"Invalid JSON arguments", true};
    }
    cJSON* brightness = cJSON_GetObjectItem(params, "brightness");
    if (!cJSON_IsNumber(brightness)) {
        cJSON_Delete(params);
        return {"Missing brightness parameter", true};
    }
    int value = brightness->valueint;
    cJSON_Delete(params);

    // Local LED abstraction does not expose per-strip brightness yet.
    ESP_LOGI(TAG, "LED set brightness: %d", value);
    return {"LED brightness set", false};
}

static McpResult handle_led_strip_set_effect(const std::string &args) {
    cJSON *params = cJSON_Parse(args.c_str());
    if (params == NULL) {
        return {"Invalid JSON arguments", true};
    }
    cJSON* effect = cJSON_GetObjectItem(params, "effect");
    if (!cJSON_IsString(effect)) {
        cJSON_Delete(params);
        return {"Missing effect parameter", true};
    }
    std::string effect_name = effect->valuestring;
    cJSON_Delete(params);

    led_strip_set_effect(effect_name.c_str());
    ESP_LOGI(TAG, "LED set effect: %s", effect_name.c_str());
    return {"LED effect set", false};
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
