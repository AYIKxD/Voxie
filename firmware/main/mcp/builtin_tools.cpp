#include "builtin_tools.h"
#include "mcp_server.h"
#include "audio/audio_service.h"
#include "system/config.h"
#include "system/state_machine.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static const char *TAG = "builtin_tools";

// Forward declarations for wifi_manager (to avoid circular deps)
extern "C" int wifi_manager_get_rssi(void);

// ============================================================================
// Tool implementations
// ============================================================================

static McpResult tool_get_device_status(const std::string &args) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"volume\":%d,"
        "\"free_heap\":%lu,"
        "\"rssi\":%d,"
        "\"uptime_s\":%llu,"
        "\"state\":\"%s\"}",
        audio_service_get_volume(),
        (unsigned long)esp_get_free_heap_size(),
        wifi_manager_get_rssi(),
        (unsigned long long)(esp_timer_get_time() / 1000000),
        device_state_to_str(state_machine_get_state()));
    return {buf, false};
}

static McpResult tool_set_volume(const std::string &args) {
    // Parse "volume" from args JSON
    const char *p = strstr(args.c_str(), "\"volume\"");
    int vol = 50;
    if (p) {
        p = strchr(p, ':');
        if (p) vol = atoi(p + 1);
    }
    audio_service_set_volume(vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "Volume set to %d%%", vol);
    return {buf, false};
}

static McpResult tool_set_brightness(const std::string &args) {
    // Parse brightness — actual LCD backlight control to be wired in display module
    const char *p = strstr(args.c_str(), "\"brightness\"");
    int brt = 50;
    if (p) {
        p = strchr(p, ':');
        if (p) brt = atoi(p + 1);
    }
    // TODO: wire to display_manager_set_brightness(brt) when display is implemented
    ESP_LOGI(TAG, "Brightness set to %d%%", brt);
    char buf[64];
    snprintf(buf, sizeof(buf), "Brightness set to %d%%", brt);
    return {buf, false};
}

static McpResult tool_set_theme(const std::string &args) {
    // Parse theme
    const char *dark = strstr(args.c_str(), "\"dark\"");
    bool is_dark = (dark != NULL);
    // TODO: wire to display_manager_set_theme() when display is implemented
    ESP_LOGI(TAG, "Theme set to %s", is_dark ? "dark" : "light");
    return {is_dark ? "Theme set to dark" : "Theme set to light", false};
}

static McpResult tool_get_system_info(const std::string &args) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const esp_app_desc_t *app_desc = esp_app_get_description();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"chip\":\"ESP32-S3\","
        "\"cores\":%d,"
        "\"flash_size_mb\":%d,"
        "\"free_heap\":%lu,"
        "\"min_free_heap\":%lu,"
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"firmware_version\":\"%s\","
        "\"compile_time\":\"%s %s\","
        "\"idf_version\":\"%s\"}",
        chip_info.cores,
        4,  // placeholder, use spi_flash_get_chip_size()/1024/1024 for actual
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        app_desc->version,
        app_desc->date, app_desc->time,
        esp_get_idf_version());
    return {buf, false};
}

static McpResult tool_reboot(const std::string &args) {
    ESP_LOGW(TAG, "Reboot requested via MCP");
    // Delay reboot slightly so the MCP response can be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return {"Rebooting...", false};  // Won't reach here
}

// ============================================================================
// Registration
// ============================================================================

void builtin_tools_register(void) {
    mcp_server_add_tool(
        "self.get_device_status",
        "Get current device status including volume, free memory, Wi-Fi RSSI, and uptime",
        R"({"type":"object","properties":{}})",
        tool_get_device_status
    );

    mcp_server_add_tool(
        "self.audio_speaker.set_volume",
        "Set the speaker volume",
        R"({"type":"object","properties":{"volume":{"type":"integer","minimum":0,"maximum":100,"description":"Volume level 0-100"}},"required":["volume"]})",
        tool_set_volume
    );

    mcp_server_add_tool(
        "self.screen.set_brightness",
        "Set the display backlight brightness",
        R"({"type":"object","properties":{"brightness":{"type":"integer","minimum":0,"maximum":100,"description":"Brightness level 0-100"}},"required":["brightness"]})",
        tool_set_brightness
    );

    mcp_server_add_tool(
        "self.screen.set_theme",
        "Switch the display UI between light and dark theme",
        R"({"type":"object","properties":{"theme":{"type":"string","enum":["light","dark"],"description":"Theme name"}},"required":["theme"]})",
        tool_set_theme
    );

    mcp_server_add_tool(
        "self.system.get_info",
        "Get detailed system information including chip, memory, MAC address, and firmware version",
        R"({"type":"object","properties":{}})",
        tool_get_system_info
    );

    mcp_server_add_tool(
        "self.system.reboot",
        "Restart the device after a 1 second delay",
        R"({"type":"object","properties":{}})",
        tool_reboot
    );

    ESP_LOGI(TAG, "Built-in MCP tools registered (%zu tools)", mcp_server_tool_count());
}
