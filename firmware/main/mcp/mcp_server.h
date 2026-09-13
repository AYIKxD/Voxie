#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <functional>
#include <string>
#include <vector>

/**
 * MCP Server — Model Context Protocol server running on the ESP32-S3.
 *
 * Implements JSON-RPC 2.0 (MCP spec 2024-11-05):
 *   - initialize: capability negotiation
 *   - tools/list: returns registered tools with JSON schemas
 *   - tools/call: validates args, invokes callback, returns result
 *
 * Tools are registered by subsystems (audio, display, IoT, camera) at init time.
 */

/// Result from a tool execution
struct McpResult {
    std::string content;    // Text content to return
    bool is_error;          // Whether this is an error result
};

/// Tool definition
struct McpTool {
    std::string name;           // e.g. "self.lamp.turn_on"
    std::string description;    // Human-readable description
    std::string input_schema;   // JSON Schema string for parameters
    std::function<McpResult(const std::string &args_json)> callback;
};

/// Initialize the MCP server
void mcp_server_init(void);

/// Register a tool. Call during init before any requests arrive.
void mcp_server_add_tool(const std::string &name,
                          const std::string &description,
                          const std::string &input_schema,
                          std::function<McpResult(const std::string &args_json)> callback);

/// Process an incoming MCP JSON-RPC request. Returns the JSON response string.
/// Called by the WebSocket text message handler when type=="mcp".
std::string mcp_server_handle_request(const std::string &request_json);

/// Get the number of registered tools
size_t mcp_server_tool_count(void);

/// Get tool list as JSON (for server-side caching)
std::string mcp_server_get_tools_json(void);
