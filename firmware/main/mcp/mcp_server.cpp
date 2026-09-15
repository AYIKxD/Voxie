#include "mcp_server.h"
#include "system/config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include <cJSON.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <memory>

static const char *TAG = "mcp_srv";

// --- Tool registry ---
static std::vector<McpTool> s_tools;

// ============================================================================
// cJSON helpers (xiaozhi-style: build responses with cJSON, own the memory)
// ============================================================================

struct CJsonDeleter {
    void operator()(cJSON *value) const {
        if (value != nullptr) cJSON_Delete(value);
    }
};
using CJsonPtr = std::unique_ptr<cJSON, CJsonDeleter>;

/// Extract a string value for a given key from a JSON object.
static std::string json_get_string(cJSON *obj, const char *key) {
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
    return value != nullptr ? value : "";
}

/// Extract an integer value for a given key.
static int json_get_int(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    return -1;
}

/// Serialize a cJSON tree to a std::string (formatted like json.dumps:
/// compact, no spaces, so server-side parsers behave identically).
static std::string json_print(cJSON *root) {
    char *text = cJSON_PrintUnformatted(root);
    if (text == nullptr) return "{}";
    std::string result(text);
    cJSON_free(text);
    return result;
}

/// Build the standard MCP response envelope:
/// {"type":"mcp","id":N,"result":{...}} or with "error":{...}
static std::string make_response(int id, CJsonPtr &&result) {
    CJsonPtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "type", "mcp");
    cJSON_AddNumberToObject(root.get(), "id", id);
    if (result) {
        cJSON_AddItemToObject(root.get(), "result", result.release());
    }
    return json_print(root.get());
}

static std::string make_error(int id, int code, const std::string &message) {
    CJsonPtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "type", "mcp");
    cJSON_AddNumberToObject(root.get(), "id", id);
    cJSON *err = cJSON_AddObjectToObject(root.get(), "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message.c_str());
    return json_print(root.get());
}

// ============================================================================
// MCP request handlers
// ============================================================================

static std::string handle_initialize(int id) {
    const esp_app_desc_t *app_desc = esp_app_get_description();

    CJsonPtr result(cJSON_CreateObject());
    cJSON_AddStringToObject(result.get(), "protocolVersion", "2024-11-05");
    cJSON *caps = cJSON_AddObjectToObject(result.get(), "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    cJSON *info = cJSON_AddObjectToObject(result.get(), "serverInfo");
    cJSON_AddStringToObject(info, "name", "Voxie");
    cJSON_AddStringToObject(info, "version", app_desc->version);

    return make_response(id, std::move(result));
}

static std::string handle_tools_list(int id, cJSON *request) {
    // Support pagination via cursor. Keep the std::string alive for the
    // duration of the parse (c_str() on a temporary would dangle).
    std::string cursor_str = json_get_string(request, "cursor");
    int cursor = cursor_str.empty() ? 0 : atoi(cursor_str.c_str());
    if (cursor < 0) cursor = 0;
    const int page_size = 10;

    CJsonPtr result(cJSON_CreateObject());
    cJSON *tools = cJSON_AddArrayToObject(result.get(), "tools");

    int count = 0;
    for (int i = cursor; i < (int)s_tools.size() && count < page_size; i++, count++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name.c_str());
        cJSON_AddStringToObject(tool, "description", s_tools[i].description.c_str());
        cJSON *schema = cJSON_Parse(s_tools[i].input_schema.c_str());
        if (schema == nullptr) {
            schema = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    int next = cursor + count;
    if (next < (int)s_tools.size()) {
        char next_buf[24];
        snprintf(next_buf, sizeof(next_buf), "%d", next);
        cJSON_AddStringToObject(result.get(), "nextCursor", next_buf);
    }

    return make_response(id, std::move(result));
}

static std::string handle_tools_call(int id, cJSON *request) {
    cJSON *params = cJSON_GetObjectItem(request, "params");
    if (params == nullptr) params = cJSON_GetObjectItem(request, "arguments");
    if (params == nullptr) {
        return make_error(id, -32602, "Missing params");
    }

    std::string tool_name = json_get_string(params, "name");

    cJSON *args = cJSON_GetObjectItem(params, "arguments");
    char *args_text = args != nullptr ? cJSON_PrintUnformatted(args) : nullptr;
    std::string arguments = args_text != nullptr ? args_text : "{}";
    if (args_text != nullptr) cJSON_free(args_text);

    ESP_LOGI(TAG, "Tool call: %s args=%s", tool_name.c_str(), arguments.c_str());

    for (const auto &tool : s_tools) {
        if (tool.name == tool_name) {
            McpResult result = tool.callback(arguments);

            CJsonPtr out(cJSON_CreateObject());
            cJSON *content = cJSON_AddArrayToObject(out.get(), "content");
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "type", "text");
            cJSON_AddStringToObject(item, "text", result.content.c_str());
            cJSON_AddItemToArray(content, item);
            cJSON_AddBoolToObject(out.get(), "isError", result.is_error);

            return make_response(id, std::move(out));
        }
    }

    CJsonPtr out(cJSON_CreateObject());
    cJSON *content = cJSON_AddArrayToObject(out.get(), "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    std::string msg = "Tool not found: " + tool_name;
    cJSON_AddStringToObject(item, "text", msg.c_str());
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(out.get(), "isError", true);

    return make_response(id, std::move(out));
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
    CJsonPtr root(cJSON_Parse(request_json.c_str()));
    if (root == nullptr) {
        return make_error(0, -32700, "Parse error");
    }

    int id = json_get_int(root.get(), "id");
    std::string method = json_get_string(root.get(), "method");

    // The cloud MCP client prefixes methods with "mcp:" (e.g. "mcp:tools/call").
    // Normalize to the bare method name used by the handlers.
    if (method.rfind("mcp:", 0) == 0) {
        method = method.substr(4);
    }

    ESP_LOGI(TAG, "MCP request: method=%s id=%d", method.c_str(), id);

    if (method == "initialize") {
        return handle_initialize(id);
    } else if (method == "tools/list") {
        return handle_tools_list(id, root.get());
    } else if (method == "tools/call") {
        return handle_tools_call(id, root.get());
    }
    return make_error(id, -32601, "Method not found: " + method);
}

size_t mcp_server_tool_count(void) {
    return s_tools.size();
}

std::string mcp_server_get_tools_json(void) {
    CJsonPtr arr(cJSON_CreateArray());
    for (const auto &t : s_tools) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", t.name.c_str());
        cJSON_AddStringToObject(item, "description", t.description.c_str());
        cJSON *schema = cJSON_Parse(t.input_schema.c_str());
        if (schema == nullptr) {
            schema = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(item, "inputSchema", schema);
        cJSON_AddItemToArray(arr.get(), item);
    }
    return json_print(arr.get());
}
