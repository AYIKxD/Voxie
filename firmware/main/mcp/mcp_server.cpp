#include "mcp_server.h"
#include "system/config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

static const char *TAG = "mcp_srv";

// --- Tool registry ---
static std::vector<McpTool> s_tools;

// ============================================================================
// JSON helpers (minimal, no external JSON library dependency)
// ============================================================================

/// Extract a string value for a given key from a JSON object string.
/// Very basic parser — sufficient for the flat MCP messages we handle.
static std::string json_get_string(const std::string &json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find('"', pos + strlen(search));
    if (pos == std::string::npos) return "";
    pos++; // skip opening quote

    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";

    return json.substr(pos, end - pos);
}

/// Extract an integer value for a given key
static int json_get_int(const std::string &json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    size_t pos = json.find(search);
    if (pos == std::string::npos) return -1;

    pos += strlen(search);
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return atoi(json.c_str() + pos);
}

/// Extract the "arguments" or "params" sub-object as a raw JSON string
static std::string json_get_object(const std::string &json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "{}";

    pos += strlen(search);
    while (pos < json.size() && json[pos] != '{') pos++;
    if (pos >= json.size()) return "{}";

    // Find matching closing brace
    int depth = 0;
    size_t start = pos;
    for (; pos < json.size(); pos++) {
        if (json[pos] == '{') depth++;
        else if (json[pos] == '}') {
            depth--;
            if (depth == 0) return json.substr(start, pos - start + 1);
        }
    }
    return "{}";
}

// ============================================================================
// MCP request handlers
// ============================================================================

static std::string handle_initialize(int id) {
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"mcp\",\"id\":%d,\"result\":{"
        "\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"Voxie\",\"version\":\"%s\"}"
        "}}",
        id, app_desc->version);
    return std::string(buf);
}

static std::string handle_tools_list(int id, const std::string &request) {
    // Support pagination via cursor
    std::string cursor_str = json_get_string(request, "cursor");
    int cursor = cursor_str.empty() ? 0 : atoi(cursor_str.c_str());
    const int page_size = 10;

    std::string tools_json = "[";
    int count = 0;
    for (int i = cursor; i < (int)s_tools.size() && count < page_size; i++, count++) {
        if (count > 0) tools_json += ",";
        tools_json += "{";
        tools_json += "\"name\":\"" + s_tools[i].name + "\",";
        tools_json += "\"description\":\"" + s_tools[i].description + "\",";
        tools_json += "\"inputSchema\":" + s_tools[i].input_schema;
        tools_json += "}";
    }
    tools_json += "]";

    std::string result = "{\"type\":\"mcp\",\"id\":" + std::to_string(id) + ",\"result\":{\"tools\":" + tools_json;

    // Add nextCursor if there are more tools
    int next = cursor + count;
    if (next < (int)s_tools.size()) {
        result += ",\"nextCursor\":\"" + std::to_string(next) + "\"";
    }
    result += "}}";

    return result;
}

static std::string handle_tools_call(int id, const std::string &request) {
    // Extract tool name from params
    std::string params = json_get_object(request, "params");
    std::string tool_name = json_get_string(params, "name");
    std::string arguments = json_get_object(params, "arguments");

    ESP_LOGI(TAG, "Tool call: %s args=%s", tool_name.c_str(), arguments.c_str());

    // Find the tool
    for (const auto &tool : s_tools) {
        if (tool.name == tool_name) {
            McpResult result = tool.callback(arguments);

            char buf[1024];
            snprintf(buf, sizeof(buf),
                "{\"type\":\"mcp\",\"id\":%d,\"result\":{"
                "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],"
                "\"isError\":%s"
                "}}",
                id,
                result.content.c_str(),
                result.is_error ? "true" : "false");
            return std::string(buf);
        }
    }

    // Tool not found
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"mcp\",\"id\":%d,\"result\":{"
        "\"content\":[{\"type\":\"text\",\"text\":\"Tool not found: %s\"}],"
        "\"isError\":true"
        "}}",
        id, tool_name.c_str());
    return std::string(buf);
}

// ============================================================================
// Public API
// ============================================================================

void mcp_server_init(void) {
    s_tools.clear();
    ESP_LOGI(TAG, "MCP server initialized (protocol version 2024-11-05)");
}

void mcp_server_add_tool(const std::string &name,
                          const std::string &description,
                          const std::string &input_schema,
                          std::function<McpResult(const std::string &args_json)> callback) {
    McpTool tool = {name, description, input_schema, callback};
    s_tools.push_back(tool);
    ESP_LOGI(TAG, "Registered tool: %s (%zu total)", name.c_str(), s_tools.size());
}

std::string mcp_server_handle_request(const std::string &request_json) {
    int id = json_get_int(request_json, "id");
    std::string method = json_get_string(request_json, "method");

    ESP_LOGI(TAG, "MCP request: method=%s id=%d", method.c_str(), id);

    if (method == "initialize") {
        return handle_initialize(id);
    } else if (method == "tools/list") {
        return handle_tools_list(id, request_json);
    } else if (method == "tools/call") {
        return handle_tools_call(id, request_json);
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"mcp\",\"id\":%d,\"error\":{"
            "\"code\":-32601,\"message\":\"Method not found: %s\"}}",
            id, method.c_str());
        return std::string(buf);
    }
}

size_t mcp_server_tool_count(void) {
    return s_tools.size();
}

std::string mcp_server_get_tools_json(void) {
    std::string json = "[";
    for (size_t i = 0; i < s_tools.size(); i++) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"name\":\"" + s_tools[i].name + "\",";
        json += "\"description\":\"" + s_tools[i].description + "\",";
        json += "\"inputSchema\":" + s_tools[i].input_schema;
        json += "}";
    }
    json += "]";
    return json;
}
