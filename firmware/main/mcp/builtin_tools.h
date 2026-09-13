#pragma once

/**
 * Built-in MCP tools — always-available device control tools.
 *
 * Registers:
 *   self.get_device_status
 *   self.audio_speaker.set_volume
 *   self.screen.set_brightness
 *   self.screen.set_theme
 *   self.system.get_info
 *   self.system.reboot
 */

/// Register all built-in MCP tools. Call after mcp_server_init().
void builtin_tools_register(void);
