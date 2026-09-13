#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the IoT tools module (GPIO, LED strip).
 */
void iot_tools_init(void);

/**
 * @brief Register all IoT tools with the MCP server.
 */
void iot_tools_register(void);

#ifdef __cplusplus
}
#endif
