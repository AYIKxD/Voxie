# Adopting XiaoZhi features into Voxie

Voxie deliberately borrows ideas from [XiaoZhi](https://github.com/78/xiaozhi-esp32)
(referred to as `xiaozhi/` in this repo, which is git-ignored). This document
tracks what we picked, what we skipped, and why — so each change can be traced
to a commit and reverted independently.

Voxie targets a **single board** and keeps the judged KWS/latency path isolated,
so board-abstraction and multi-transport ideas from XiaoZhi are intentionally
out of scope (see `build.md` §0.3).

## Adopted

| # | Item | XiaoZhi source | Voxie commit |
|---|------|----------------|--------------|
| 1 | NVS-backed `Settings` key/value store | `main/settings.{h,cc}` | `3253822` |
| 2 | cJSON-based WebSocket message dispatch | `main/application.cc`, `cjson_utils.h` | `ac8d6d9` |
| 3 | cJSON-based MCP server request/response handling | `main/mcp_server.cc` | `423db23` |
| 4 | `self.upgrade_firmware` MCP tool (OTA) | `mcp_server.cc` (`AddUserOnlyTools`) | `9a5c956` |
| 5 | Persistent speaker volume | `Settings` usage in `application` | `caaac47` |
| 6 | Recover to idle when the audio channel closes | `Application::OnAudioChannelClosed` | `e24c583` |
| 7 | Friendly DHCP hostname (`Voxie-XXXX`) | `boards/common/wifi_board.cc` | `9ae159b` |
| 8 | Real flash size in system info | `SystemInfo::GetFlashSize` | `3c78ddc` |
| 9 | Per-task CPU usage / heap reporting | `SystemInfo::PrintTaskCpuUsage` | `fced5f5` |
| 10 | Async spoken notifications | `notify/notify_player.*` | `de0f3eb`, `55a9810` |
| 11 | Skip redundant UI state updates | `Led::OnStateChanged` / state listeners | `b468440` |

### Notes / intentional deviations

- **Notifications (10):** XiaoZhi downloads an `audio_url` (Ogg) and demuxes it.
  Voxie instead streams the spoken text as Opus binary frames on the existing
  TTS pipeline, avoiding a separate OGG demuxer and second decoder. Wire
  format: `{"type":"notify","state":"start","text":...}` → binary TTS frames →
  `{"type":"notify","state":"end"}`.
- **State machine (11):** XiaoZhi uses a listener/observer registry; Voxie keeps
  the lighter C state machine but makes `display_manager_set_state` and
  `led_strip_set_state` idempotent, which removes the same per-second redraw
  cost without the observer plumbing.
- **OTA (4):** only URL-driven upgrade is adopted. XiaoZhi's server version
  check + activation challenge requires a matching cloud endpoint that Voxie's
  FastAPI server does not expose yet.

## Analyzed but deferred

| Item | XiaoZhi source | Why deferred |
|------|----------------|--------------|
| MQTT/UDP transport | `protocols/mqtt_protocol.*` | Explicitly not adopted in `build.md` §0.3; single WebSocket is sufficient. |
| Board abstraction (100+ boards) | `boards/**` | Voxie deliberately targets one board. |
| BluFi (BLE) Wi-Fi provisioning | `boards/common/blufi.*` | SoftAP captive portal already ships; BLE stack adds flash/RAM. Needs hardware validation. |
| Power-save timer / light sleep | `boards/common/power_save_timer.*` | Requires `esp_pm` + WiFi coexistence tuning; risky to change without hardware test. High value for the efficiency criterion — revisit with a device. |
| ADC/AXP2101 battery monitoring | `boards/common/*battery*` | Voxie hardware has no battery gauge wired. |
| On-device / server-side AEC modes | `AecMode`, AFE wake-word engine | Server-side AEC needs a host DSP pipeline. ESP-SR AFE is **disabled** (see commit note below): it needs a proprietary WakeNet `model` partition and would replace our open-source TFLite KWS. |
| UDP audio debugger | `main/audio/audio_debugger.*` | Useful for KWS tuning; needs a host listener tool. Candidate next. |
| LVGL/OLED rendering | `main/display/**` | `display_manager` is still log/stub-only; a full display stack is a larger, hardware-validated effort. |
| Dynamic text glyph push | `docs/glyph-push.md` | Only relevant with a real display stack. |
| MCP `press_to_talk` / wake-word mute tools | `boards/common/press_to_talk_mcp_tool.*` | Low demo value; adds a persisted mode. |

## Verification status

All adopted changes build cleanly with ESP-IDF v6.1 (`idf.py build`,
target `esp32s3`). Changes that alter runtime audio/UI behavior (volume
persistence, notifications, disconnect recovery) have **not** been validated on
physical hardware yet.

## Known issue fixed: AFE disabled

Commit `722349f` wired ESP-SR AFE into the mic path: when `VOXIE_HAS_AFE=1`,
`kws_engine` reads audio only from `kws_audio_ringbuf`, which is fed solely by
the AFE task. AFE needs a `model` partition (`esp_srmodel_init("model")`) that
this project does not ship, so AFE init failed, its task never started, and
nothing fed KWS or `stream_service` — the wake word never fired.

`VOXIE_HAS_AFE` is now `0` (the default in `config.h`): `kws_engine` reads the
I2S mic directly and forwards audio to `stream_service`, which restores wake
word detection. This also matches `build.md` §0.3, which explicitly rejects
ESP-SR/WakeNet in favor of the TFLite microWakeWord engine.

