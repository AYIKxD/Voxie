# BUILD.md — Voxie: Edge Wake-Word + MCP-Powered AI Voice Assistant
### SIH: "Low Latency and Efficient Voice Activator for Edge Devices"

This document is a complete implementation spec. Hand it to a coding agent (or follow it
yourself) to build the entire system: on-device keyword spotting (KWS), low-latency audio
streaming to a cloud ASR server, and — built on top of the judged pipeline — a full-featured
AI voice assistant with **MCP (Model Context Protocol)** extensibility, IoT device control,
rich LCD UI, Opus-compressed audio, OTA updates, and camera vision.

Read **Section 0** first. It maps every judging criterion to a concrete design decision so
nothing gets built that doesn't directly serve the evaluation. Then read **Section 1** for
the full architectural vision that makes this project dramatically more impressive than a
bare wake-word demo.

---

## 0. Scope, compliance map, and non-negotiables

### 0.1 What is actually judged vs. what is demo excellence

The problem statement grades exactly **two things**: the KWS engine (efficiency + accuracy)
and the keyword-end → cloud-ASR-received latency. Everything after "ASR receives audio" —
LLM reply generation, TTS, speaker playback, MCP tool calls, IoT control, display UI — is
**not** part of the graded pipeline.

However, the demo layer is what separates "technically correct" from "judges remember you a
week later." XiaoZhi proved that an ESP32-S3 can run a full MCP server, Opus codec, LVGL
display, camera vision, and IoT control simultaneously. We adopt every one of those ideas
that adds demo impact, while keeping the judged KWS path architecturally isolated and
independently benchmarkable.

**Two-tier architecture rule:** the judged pipeline (KWS → latency measurement) must work
perfectly with the demo layer entirely disabled. Every demo feature is behind a compile-time
flag (`VOXIE_ENABLE_*`) so the judged path can be isolated for clean benchmarking.

### 0.2 Compliance map

| Judged requirement | How this build satisfies it |
|---|---|
| Open-source only, no proprietary voice-activation SDK | KWS uses TensorFlow Lite for Microcontrollers (Apache-2.0) + Espressif's `esp-tflite-micro`/`esp-nn` (Apache-2.0/MIT) + the `microWakeWord` training framework (open source). We explicitly **do not** use Espressif's ESP-SR/Skainet (WakeNet/MultiNet) — its shipped wake-word models are closed, pre-trained binaries, which is exactly what disqualified XiaoZhi's approach. We also do not use Picovoice Porcupine or any other commercial KWS SDK. |
| No pre-trained global keywords ("Hey Google", "Alexa") | We train a brand-new model from scratch on your own chosen phrase using the `microWakeWord` pipeline. We never load its pre-shipped `hey_jarvis` / `alexa` / `ok_nabu` release models — those are reference examples only. |
| Model efficiency: RAM/flash footprint, idle CPU <10% | int8-quantized streaming model, tens of KB flash, tensor arena on the order of tens of KB RAM, run through `esp-nn`-accelerated kernels. Inference runs once per 10–20 ms audio stride on a ~40-value feature vector, not on raw audio — this is what keeps idle CPU low. Measured directly (see §10). |
| Accuracy: high true-positive, near-zero false-accept | Handled by the `microWakeWord` training methodology: large synthetic multi-speaker dataset + heavy augmentation + explicit hard-negative mining, with a tunable detection threshold and smoothing window traded off against your measured false-accept rate (see §4 and §10). |
| Latency: keyword-end → cloud ASR receiving audio | SNTP-synchronized clocks + a timestamp embedded in the first streamed packet, measured server-side (see §8.3). This gives you a defensible, judge-presentable number instead of a guess. |
| Evaluated on a real low-power MCU | Everything on-device is written in ESP-IDF (C/C++) for the ESP32-S3, not emulated. |

### 0.3 What we take from XiaoZhi (and what we don't)

| Adopted from XiaoZhi | Why |
|---|---|
| **MCP Server on the MCU** | Turns the device from a dumb mic/speaker into an extensible AI agent platform. The LLM can discover and call device functions (adjust volume, control LEDs, read sensors, take photos) dynamically. This is the single most impressive demo feature. |
| **Opus codec** instead of raw PCM streaming | ~10× bandwidth reduction (PCM 16kHz mono = 256 kbps vs Opus = 16–24 kbps). Critical for real-world Wi-Fi reliability and makes the system viable over cellular. |
| **LVGL LCD display** with themes & chat bubbles | An SSD1306 OLED showing "LISTENING..." is fine for judging; a color LCD with chat bubbles, dark/light themes, and animated states wins the demo. |
| **Barge-in / continuous wake-word detection** | KWS keeps running during playback so users can interrupt the assistant mid-sentence. Natural UX. |
| **IoT device control via MCP tools** | "Hey Voxie, turn on the desk lamp" → LLM calls `self.lamp.turn_on` via MCP → GPIO fires. Judges love tangible real-world demos. |
| **OTA firmware updates** | Dual-partition OTA with rollback. Professional-grade, and useful during development. |
| **Wi-Fi provisioning via SoftAP** | No hardcoded Wi-Fi credentials. Device hosts a captive portal for setup. |
| **Async voice notifications** | Cloud can push spoken alerts to an idle device without the user initiating. |
| **Camera + vision** (stretch) | "Hey Voxie, what's on this whiteboard?" → captures photo → sends to vision LLM → speaks answer. |

| **NOT** adopted from XiaoZhi | Why |
|---|---|
| ESP-SR / WakeNet / MultiNet for wake words | Proprietary pre-trained models. Violates the SIH open-source requirement. This is the core differentiator of our build. |
| MQTT + UDP hybrid transport | Over-engineered for a demo. Single WebSocket is simpler and sufficient. |
| 138-board hardware abstraction | We target one board. Premature abstraction. |
| Dynamic glyph push | Clever, but we'll use a font with good coverage baked into flash. |
| Cellular modem support | Demo is on Wi-Fi. |

### 0.4 Fill these in before/while building

The agent building this should treat these as config constants, not hardcode them inline:

```
WAKE_WORD            = "<your chosen phrase, e.g. 'Hey Voxie'>"   # 2–3 syllables, not a common word
ESP32S3_VARIANT      = "<e.g. ESP32-S3-WROOM-1 N16R8 — confirm PSRAM presence>"
CLOUD_HOST           = "<e.g. your EC2 public IP or domain>"
LLM_PROVIDER         = "openai" | "anthropic" | "gemini"   # whichever key you hold
DISPLAY_TYPE         = "lcd_st7789" | "oled_ssd1306"        # what you have wired
VOXIE_ENABLE_MCP     = 1       # MCP server on device
VOXIE_ENABLE_IOT     = 1       # IoT tool registration
VOXIE_ENABLE_CAMERA  = 0       # set 1 if camera module present
VOXIE_ENABLE_OTA     = 1       # OTA update support
```

> **PSRAM assumption:** this build assumes your ESP32-S3 module has PSRAM (most audio-capable
> dev boards do — WROOM-1 N8R2/N8R8/N16R8, Korvo, etc.). PSRAM is what makes it comfortable to
> hold Wi-Fi buffers, Opus encoder state, LVGL frame buffers, I2S DMA buffers, and the TFLite
> tensor arena simultaneously. If your exact module has **no** PSRAM, see the callout in §6.10.

---

## 1. System architecture

### 1.1 High-level data flow

```
                     ┌──────────────────────────────────────────────────────────┐
                     │                    ESP32-S3 firmware                      │
                     │                                                          │
 INMP441/ICS43434 ──▶│  I2S RX ──▶ ring buf ──▶ feature extractor              │
   (I2S mic)         │                               │                          │
                     │                               ▼                          │
                     │                  streaming KWS inference                 │
                     │                  (TFLite Micro + ESP-NN)                 │
                     │                               │ detect                   │
                     │                               ▼                          │
                     │     pre-roll buf + mic ──▶ Opus encoder ──▶ WebSocket ───┼──▶ Wi-Fi
                     │                                                │         │
                     │     ┌──── MCP Server (JSON-RPC 2.0) ◀──────────┤         │
                     │     │         │                                │         │
                     │     │    IoT tools    Device tools             │         │
                     │     │   (lamp,LED,    (volume,display,         │         │
                     │     │    sensor)       brightness,camera)      │         │
                     │     │                                          │         │
                     │     │         Opus decoder ◀── TTS audio ◀────┘         │
                     │     │              │                                     │
      MAX98357A ◀────┤  I2S TX ◀── playback ring buffer                        │
      + 3W speaker   │                                                          │
                     │  LCD (SPI) / OLED (I2C) ◀── LVGL UI engine              │
                     │     chat bubbles │ status bar │ themes │ animations      │
                     └──────────────────────────────────────────────────────────┘
                                          │  WebSocket (Wi-Fi)
                                          ▼
                     ┌──────────────────────────────────────────────────────────┐
                     │            Cloud backend (Docker, self-hosted)            │
                     │                                                          │
                     │  WS ingress ──▶ Opus decode ──▶ VAD end-pointing         │
                     │       faster-whisper (ASR) ──▶ transcript                │
                     │                                    │                     │
                     │                                    ▼                     │
                     │                          LLM (your API key)              │
                     │                            + MCP Client                  │
                     │                                    │                     │
                     │                         ┌──────────┼──────────┐          │
                     │                         │          │          │          │
                     │                    reply text  MCP calls  vision req     │
                     │                         │          │          │          │
                     │                         ▼          ▼          ▼          │
                     │                    Piper TTS   WS→device  Vision LLM    │
                     │                         │                                │
                     │                  Opus encode ──▶ WS egress               │
                     └──────────────────────────────────────────────────────────┘
```

### 1.2 Device state machine

```
                                    ┌──────────────────┐
                                    │   UNPROVISIONED   │
                                    │  (SoftAP portal)  │
                                    └────────┬─────────┘
                                             │ Wi-Fi configured
                                             ▼
                                    ┌──────────────────┐
                                    │   IDLE_LISTENING   │◀──────────────────────┐
                                    │  (KWS running,     │                       │
                                    │   display idle)    │◀─────────┐            │
                                    └────────┬─────────┘           │            │
                                             │ wake word detected  │            │
                                             ▼                     │            │
                                    ┌──────────────────┐           │            │
                                    │ STREAMING_TO_ASR  │           │            │
                                    │ (Opus↑, KWS still │           │            │
                                    │  running for       │           │            │
                                    │  barge-in)         │           │            │
                                    └────────┬─────────┘           │            │
                                             │ server VAD: EOS     │            │
                                             ▼                     │            │
                                    ┌──────────────────┐           │            │
                                    │ WAITING_FOR_REPLY │           │            │
                                    │ (thinking anim)   │           │            │
                                    └────────┬─────────┘           │            │
                                             │ TTS starts          │            │
                                             ▼                     │            │
                                    ┌──────────────────┐           │            │
                                    │  PLAYING_REPLY    │───────────┘            │
                                    │  (KWS still runs  │  wake word = barge-in │
                                    │   for barge-in)   │                       │
                                    └────────┬─────────┘                        │
                                             │ TTS ends                         │
                                             └──────────────────────────────────┘

Async paths:
  IDLE_LISTENING ──(cloud push notification)──▶ NOTIFYING ──(done)──▶ IDLE_LISTENING
  Any state ──(OTA command via MCP)──▶ UPGRADING ──(reboot)──▶ IDLE_LISTENING
```

Key difference from beta plan: KWS inference **never stops** — it runs during playback too,
enabling natural barge-in. This is how XiaoZhi does it and users expect it.

### 1.3 Why this specific KWS design

The `esphome/esphome` project's `micro_wake_word` component and its underlying training
repo (`kahrendt/microWakeWord`, now maintained at `OHF-Voice/micro-wake-word`) already solve
the hard parts of this exact problem: it's open source, explicitly built to run on ESP32-S3
under tight RAM/CPU budgets, trained without Espressif's proprietary Skainet, and supports
training an arbitrary custom phrase without needing samples from real speakers (it
synthesizes them). It uses a streaming architecture based on Google's "Streaming Keyword
Spotting on Mobile Devices" paper (Rykabov, Kononenko, Subrahmanya, Visontai, Laurenzo),
converted to run as small, fast repeated inferences on a sliding window of audio features
rather than one big inference over raw audio.

We reuse its **training methodology and feature-extraction approach**, but run inference in
our **own** ESP-IDF firmware (not inside ESPHome) using `espressif/esp-tflite-micro`'s
`micro_speech` example as the native skeleton — that example already wires up TFLite Micro,
the audio front-end feature generator, and `esp-nn` acceleration for ESP-IDF directly. Both
projects use the same canonical TFLM audio front-end (the "micro_speech preprocessor"), so a
model trained via `microWakeWord` is a drop-in replacement for `micro_speech`'s demo model —
we're not reinventing feature extraction, just swapping the classifier.

### 1.4 Why MCP is the killer feature (learned from XiaoZhi)

XiaoZhi demonstrated that an ESP32-S3 can run a full **MCP (Model Context Protocol) server**
directly on the microcontroller. This transforms the device from a dumb audio pipe into an
**AI agent platform**:

1. **Tool Discovery**: When the LLM connects, it queries `tools/list` and learns what the
   device can do — no hardcoded command lists, no rigid voice-command grammars.
2. **Dynamic Control**: The LLM calls `tools/call` with structured JSON arguments to control
   hardware: adjust volume, change display theme, toggle a lamp, read a sensor, take a photo.
3. **Extensibility**: Adding a new IoT device = registering one new tool with a name, JSON
   schema, and callback. No firmware recompilation for the core system, just the tool module.
4. **Standard Protocol**: MCP is Anthropic's open standard (spec 2024-11-05), supported by
   Claude, ChatGPT, Gemini, and open-source LLMs. Any MCP-aware LLM can control the device
   out of the box.

This is what makes judges say "this isn't just a wake-word demo, it's a product."

---

## 2. Hardware & wiring

### 2.1 Core components

| Component | Interface | Notes |
|---|---|---|
| ESP32-S3 (dev board) | — | Confirm PSRAM (see §0.4). Dual I2S peripherals used: I2S0 for mic RX, I2S1 for speaker TX. |
| ICS43434 (primary) or INMP441 (fallback) | I2S, 16 kHz / 16-bit mono | Pin names identical between the two — no firmware change needed to swap. Tie L/R to GND for left-channel/mono capture. |
| MAX98357A | I2S TX | Separate I2S peripheral from the mic. |
| 3W speaker | Analog, driven by MAX98357A | — |
| ST7789 LCD 1.69" 240×280 (primary) or SSD1306 OLED 0.96" (fallback) | SPI / I2C | LCD enables LVGL chat bubbles, themes, status bar. OLED is a degraded-but-functional fallback. |

### 2.2 Optional demo-impact components

| Component | Interface | Demo value |
|---|---|---|
| WS2812B LED strip/ring (8–16 LEDs) | GPIO (RMT) | Visual feedback: breathing while idle, chase while listening, pulse on reply. Controllable via MCP. |
| Relay module or smart LED bulb | GPIO | "Hey Voxie, turn on the lamp" — tangible IoT demo. |
| DHT22 / BME280 sensor | GPIO / I2C | "Hey Voxie, what's the temperature?" — sensor reading via MCP. |
| OV2640 camera module | DVP/SCCB | "Hey Voxie, what's on this paper?" — vision via MCP. Stretch goal. |
| Push button | GPIO | Manual trigger alternative to wake word (accessibility, quiet demos). |

### 2.3 Default GPIO map

```
I2S0 (mic, RX):       WS=GPIO4   SCK=GPIO5   SD=GPIO6
I2S1 (speaker, TX):   WS=GPIO15  SCK=GPIO16  SD=GPIO17
SPI LCD (ST7789):      MOSI=GPIO11  SCLK=GPIO12  CS=GPIO10  DC=GPIO13  RST=GPIO14  BL=GPIO21
I2C (OLED fallback):   SDA=GPIO8   SCL=GPIO9
WS2812B LED strip:     GPIO48
Relay / lamp:          GPIO47
DHT22 sensor:          GPIO46
Button:                GPIO0 (BOOT button, active low)
Camera (if present):   standard ESP32-CAM DVP pinout
```

Adjust to match whatever you already have wired. Keep existing mic wiring if it's soldered.

---

## 3. Repository layout

```
/firmware              ESP-IDF project (the ESP32-S3 app)
  /main
    app_main.cpp                 # entry point → Application::Initialize() + Run()
    application.cpp/.h           # central coordinator, event loop, state dispatch

    /audio
      audio_service.cpp/.h       # I2S mic RX + speaker TX + ring buffers + Opus enc/dec
      opus_encoder.cpp/.h        # libopus wrapper: 16kHz mono, 16-24kbps, 60ms frames
      opus_decoder.cpp/.h        # decodes incoming TTS Opus audio

    /kws
      kws_engine.cpp/.h          # TFLite Micro interpreter + feature extractor + smoothing
      model_data.h               # generated: trained .tflite embedded as C array

    /protocol
      protocol.cpp/.h            # abstract transport base class
      websocket_protocol.cpp/.h  # WS client: JSON text frames + binary Opus frames
      binary_protocol.h          # packed binary frame header (version, type, timestamp, size)

    /mcp
      mcp_server.cpp/.h          # JSON-RPC 2.0 MCP server: initialize, tools/list, tools/call
      builtin_tools.cpp/.h       # self.get_device_status, self.audio_speaker.set_volume, etc.
      iot_tools.cpp/.h           # self.lamp.*, self.sensor.*, self.led_strip.* (if enabled)
      camera_tool.cpp/.h         # self.camera.take_photo (if enabled)

    /display
      display_manager.cpp/.h     # abstract display interface
      lcd_display.cpp/.h         # LVGL 9.x: chat bubbles, status bar, themes, thinking anim
      oled_display.cpp/.h        # SSD1306 fallback: simple text state display
      led_strip.cpp/.h           # WS2812B animations: idle breathe, listening chase, speaking pulse

    /iot
      lamp_controller.cpp/.h     # GPIO relay control, registered as MCP tool
      sensor_reader.cpp/.h       # DHT22/BME280 readings, registered as MCP tool

    /system
      wifi_manager.cpp/.h        # SoftAP captive portal for Wi-Fi provisioning
      ota_manager.cpp/.h         # dual-partition OTA with rollback
      state_machine.cpp/.h       # device state enum + transitions + event dispatch
      config.h                   # all VOXIE_ENABLE_* flags + pin assignments + constants

    /notify
      notify_player.cpp/.h       # async cloud-pushed voice notifications

  /partitions
    partitions.csv               # app0, app1 (OTA), nvs, assets
  CMakeLists.txt
  sdkconfig.defaults

/training              Python — everything for the wake-word model
  generate_dataset.py            # synthetic positive/negative sample generation
  augment.py                     # noise/RIR/speed augmentation
  train.py                       # microWakeWord training pipeline wrapper
  export_tflite.py               # quantize + export .tflite
  evaluate.py                    # FAR/FRR reporting on a held-out set

/server                Cloud backend (Docker, self-hosted)
  app.py                         # FastAPI + WebSocket server
  asr.py                         # faster-whisper wrapper
  vad.py                         # Silero VAD end-pointing
  llm.py                         # pluggable OpenAI/Anthropic/Gemini client
  mcp_client.py                  # MCP client: forwards LLM tool calls to device over WS
  tts.py                         # Piper TTS wrapper
  opus_utils.py                  # server-side Opus encode/decode
  vision.py                      # camera image → vision LLM analysis
  notify.py                      # push notification sender
  Dockerfile
  docker-compose.yml

/docs
  latency_report.md              # filled in during §10 testing
  demo_checklist.md
  mcp_tools_reference.md         # documents all registered MCP tools
  protocol_spec.md               # WebSocket framing specification
```

---

## 4. Phase 1 — Wake-word dataset & model training

1. **Pick the phrase.** 2–3 syllables, phonetically distinct from common words in your demo
   environment (avoid words likely to appear in ambient conversation/TV during judging).
   Suggestion: **"Hey Voxie"** — short, distinct, brandable.

2. **Generate synthetic positive samples.** Follow `microWakeWord`'s approach: use Piper TTS
   (many voices, varied pitch/speed) to synthesize hundreds–thousands of renderings of the
   phrase, rather than relying solely on live recordings from your team. This is what removes
   the dependency on having many human speakers available.
   - Still record a **small set of real recordings** (each teammate, a few takes, in the
     actual room/mic setup you'll demo with) — not for training volume, but as a realistic
     validation set.

3. **Augment.** Add background noise (e.g. MUSAN or any ambient recordings from your demo
   venue), simulated room reverb (RIR convolution), and speed/pitch perturbation. This is
   what determines real-world robustness far more than raw sample count.

4. **Negative set.** Use the Google Speech Commands dataset (common short words) plus a
   "hard negative" set: words that share syllables/phonemes with your wake word, and — very
   important — several hours of the actual ambient background audio from wherever you'll be
   demoing, explicitly mined for anything that currently false-triggers.

5. **Train.** Use the `microWakeWord` training pipeline (TensorFlow). It trains a small
   streaming network on top of the same 40-feature mel-filterbank front end used by TFLM's
   `micro_speech` example, then quantizes to int8.

6. **Evaluate before exporting.** Compute false-accept rate against your ambient-noise
   negative set and false-reject rate against your held-out positive set. Tune the detection
   probability threshold and the sliding-window smoothing size.

7. **Export.** Produce the final `.tflite` file. Embed it via CMake's `EMBED_FILES` so it's
   flashed as part of the firmware image without needing a filesystem.

---

## 5. Phase 2 — Opus audio codec integration

### Why Opus instead of raw PCM (learned from XiaoZhi)

The beta plan streamed raw PCM (16kHz × 16-bit × mono = **256 kbps**). XiaoZhi uses Opus at
16–24 kbps — a **10×+ reduction**. This matters because:

- Wi-Fi at a crowded demo venue is unreliable. Lower bitrate = fewer dropped packets.
- Reduces WebSocket frame sizes from 640 bytes/20ms to ~40–60 bytes/20ms.
- Opus is specifically designed for low-latency speech (algorithmic delay as low as 5ms).
- The ESP32-S3 has more than enough CPU for real-time Opus encode/decode alongside KWS.

### Implementation

1. **Add libopus as an ESP-IDF component.** Use `espressif/esp-opus` managed component or
   compile libopus from source with the `fixed-point` and `OPUS_ARM_ASM` (or generic)
   options for the Xtensa LX7 core.

2. **Encoder settings** (device → server):
   - Sample rate: 16000 Hz
   - Channels: 1 (mono)
   - Application: `OPUS_APPLICATION_VOIP`
   - Bitrate: 16000–24000 bps
   - Frame size: 960 samples (60ms) — matches XiaoZhi's default, good latency/efficiency tradeoff
   - Complexity: 0–3 (lower = less CPU, fine for speech)

3. **Decoder settings** (server → device, TTS playback):
   - Same parameters. Opus is symmetric.
   - Feed decoded PCM directly into the I2S TX playback ring buffer.

4. **Server side**: Use Python `opuslib` or `pyogg` for encode/decode. The server encodes
   Piper TTS output into Opus before sending it back.

---

## 6. Phase 3 — ESP-IDF firmware

### 6.1 Base skeleton

Start from `espressif/esp-tflite-micro`'s `micro_speech` example (targets `esp32s3`,
already wires up ESP-NN). Replace its demo model with your trained one; keep its audio
preprocessor/feature-extraction code as-is since it's the same front end your training
pipeline targets.

### 6.2 FreeRTOS task layout

| Task | Core | Priority | Role |
|---|---|---|---|
| `i2s_mic_task` | 0 | highest | I2S RX → ring buffer. Never blocks on anything but I2S DMA. |
| `feature_task` | 0 | high | Pulls audio strides from ring buffer, computes 40-value feature frame every 10–20 ms. |
| `kws_task` | 0 | high | Runs `Interpreter::Invoke()` on each feature frame; applies smoothing/threshold; raises FreeRTOS event on detection. **Never stops** — runs during playback for barge-in. |
| `opus_encode_task` | 1 | high | On wake event: encodes mic audio into Opus frames, pushes to WebSocket TX queue. |
| `stream_task` | 1 | medium | Manages WebSocket lifecycle, sends Opus frames upstream, receives JSON control + Opus TTS downstream, dispatches MCP messages. |
| `opus_decode_task` | 1 | medium | Decodes incoming Opus TTS frames → PCM → playback ring buffer. |
| `playback_task` | 1 | medium | Feeds decoded PCM from playback ring buffer to I2S TX DMA. |
| `mcp_task` | 1 | medium | Processes MCP JSON-RPC requests: tool discovery, parameter validation, callback dispatch. |
| `display_task` | 1 | low | LVGL tick + render: chat bubbles, status bar, state animations. |
| `led_task` | 1 | lowest | WS2812B animation engine, driven by state machine transitions. |
| Wi-Fi / SNTP | — | — | Standard ESP-IDF Wi-Fi station + reconnect logic; SNTP sync on boot and periodically. |

Pin audio-critical tasks (`i2s_mic_task`, `feature_task`, `kws_task`) to Core 0. Everything
else (network, display, MCP, Opus encode/decode) runs on Core 1 so continuous listening
doesn't get starved by network activity.

### 6.3 Idle-loop resource discipline

This is what the "<10% CPU, <256KB RAM while idling" requirement is actually testing:
- Feature extraction + inference runs on a small, fixed-size buffer, at a fixed stride
  (10–20 ms) — not continuously on every audio sample.
- Don't allocate/free per-inference; allocate the tensor arena and all buffers once at boot.
- Keep `stream_task`, `opus_encode_task`, `playback_task`, and the WebSocket connection
  **idle/dormant** while in `IDLE_LISTENING` — no audio leaves the device before a wake word.
- Opus encoder state is allocated once at boot but idle (no CPU) until streaming starts.
- LVGL renders only on state changes and at a low tick rate (30ms) while idle.

### 6.4 Pre-roll buffer

Wake-word detection has inherent latency. Keep a rolling ~500 ms–1 s buffer of raw audio
behind the KWS pipeline at all times. On detection, Opus-encode and send that pre-roll first,
then live audio — this lets you legitimately timestamp "keyword end" precisely.

### 6.5 Barge-in (continuous wake-word detection)

Unlike the beta plan's "stretch goal" note, barge-in is now a **core feature**:

1. KWS inference runs continuously in `kws_task`, even during `PLAYING_REPLY` state.
2. When a wake word fires during playback:
   - Send `{"type":"abort","reason":"wake_word_detected"}` to server.
   - Immediately stop Opus decode and flush the playback buffer.
   - Transition to `STREAMING_TO_ASR`.
3. The challenge is acoustic echo: the speaker is playing audio while the mic is listening.
   **Mitigation**: the wake-word model, trained on heavily augmented audio (including
   synthetic echo), is naturally somewhat resistant. For additional robustness, mute/duck
   the speaker momentarily when a possible detection is in progress (the smoothing window
   gives you a few frames of warning).

### 6.6 MCP Server implementation

The on-device MCP server handles JSON-RPC 2.0 messages received via the WebSocket:

```
Incoming JSON with "type":"mcp" → dispatch to McpServer

McpServer supports:
  "initialize"  → returns protocol version, capabilities {tools:{}}, server info
  "tools/list"  → returns registered tools with JSON schemas, supports pagination (cursor)
  "tools/call"  → validates arguments against schema, invokes C++ callback, returns result
```

**Built-in tools** (always registered):

| Tool name | Description | Parameters |
|---|---|---|
| `self.get_device_status` | Returns volume, brightness, free heap, Wi-Fi RSSI, battery%, uptime | none |
| `self.audio_speaker.set_volume` | Adjusts speaker volume | `volume: int [0–100]` |
| `self.screen.set_brightness` | Adjusts LCD backlight | `brightness: int [0–100]` |
| `self.screen.set_theme` | Switches UI theme | `theme: "light" \| "dark"` |
| `self.system.get_info` | Returns chip, flash, RAM, MAC, firmware version | none |
| `self.system.reboot` | Restarts device after 1s delay | none |
| `self.system.upgrade_firmware` | OTA from URL | `url: string` |

**IoT tools** (registered if `VOXIE_ENABLE_IOT=1`):

| Tool name | Description | Parameters |
|---|---|---|
| `self.lamp.turn_on` | Energizes relay GPIO | none |
| `self.lamp.turn_off` | De-energizes relay GPIO | none |
| `self.lamp.get_state` | Returns on/off state | none |
| `self.sensor.read_temperature` | Reads DHT22/BME280 | none |
| `self.sensor.read_humidity` | Reads DHT22/BME280 | none |
| `self.led_strip.set_color` | Sets all LEDs to a color | `r,g,b: int [0–255]` |
| `self.led_strip.set_brightness` | Adjusts LED strip brightness | `brightness: int [0–100]` |
| `self.led_strip.set_effect` | Runs an animation | `effect: "rainbow" \| "breathe" \| "chase" \| "off"` |

**Camera tool** (registered if `VOXIE_ENABLE_CAMERA=1`):

| Tool name | Description | Parameters |
|---|---|---|
| `self.camera.take_photo` | Captures JPEG, uploads to vision endpoint, returns description | `question: string` |

**Registration pattern** (C++ side):
```cpp
mcp_server.AddTool(
    "self.lamp.turn_on",
    "Turn on the desk lamp",
    R"({"type":"object","properties":{}})",  // JSON schema
    [](const json& args) -> McpResult {
        gpio_set_level(LAMP_GPIO, 1);
        return {.content = "Lamp turned on", .is_error = false};
    }
);
```

### 6.7 Display system

**LCD mode (ST7789, primary):**
- Use LVGL 9.x via the `esp_lvgl_port` managed component.
- Status bar (top): Wi-Fi RSSI icon, battery %, time (SNTP-synced), mute indicator.
- Main area: chat bubble UI showing user speech (STT transcripts from server) and assistant
  replies. Light and dark themes switchable via MCP.
- State animations: pulsing mic icon while listening, "thinking..." dots while waiting,
  waveform while speaking.
- LVGL renders into a PSRAM-allocated frame buffer, DMA'd to the SPI LCD.

**OLED mode (SSD1306, fallback):**
- Simple state text: "Listening...", "Processing...", "Speaking...", plus a scrolling
  one-line transcript.

**LED strip:**
- Idle: slow blue breathe.
- Listening: green chase/spin.
- Processing: yellow pulse.
- Speaking: white gentle breathe.
- Error: red flash.
- All colors/effects controllable via MCP tool `self.led_strip.set_effect`.

### 6.8 Wi-Fi provisioning (SoftAP captive portal)

On first boot (no stored Wi-Fi credentials in NVS):

1. Device starts in `UNPROVISIONED` state.
2. Creates a SoftAP: `Voxie-XXXX` (last 4 of MAC).
3. Runs a minimal HTTP server serving a single-page HTML form:
   - Scans and lists nearby Wi-Fi networks.
   - User selects network, enters password.
   - Optionally configure cloud server URL.
4. Credentials saved to NVS. Device reboots into normal mode.
5. If stored Wi-Fi fails to connect after N retries, falls back to SoftAP provisioning again.

This eliminates hardcoded Wi-Fi credentials — essential for a product-quality demo.

### 6.9 OTA firmware updates

Dual-partition layout (see `/partitions/partitions.csv`):

```
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
app0,     app,  ota_0,   0x10000,  0x1E0000,
app1,     app,  ota_1,   0x1F0000, 0x1E0000,
nvs_keys, data, nvs_keys,0x3D0000, 0x1000,
assets,   data, spiffs,  0x3D1000, 0x2F000,
```

OTA flow:
1. LLM calls MCP tool `self.system.upgrade_firmware` with a URL.
2. Device downloads the binary to the inactive OTA partition.
3. Validates the image, marks it bootable, and reboots.
4. On successful boot, cancels rollback. On crash, automatic rollback to previous partition.

### 6.10 No-PSRAM variant

If your exact module lacks PSRAM: reduce pre-roll buffer to ~500 ms, drop I2S DMA buffer
count/size to minimum stable value, use single LVGL frame buffer (or switch to OLED-only),
keep Opus complexity at 0, and confirm the tensor arena fits in internal SRAM alongside the
Wi-Fi stack.

---

## 7. Phase 4 — Cloud backend

### 7.1 Hosting

Self-hosted, Docker-deployed:
- CPU-only is workable with faster-whisper's INT8 CTranslate2 backend on a compute-optimized
  instance (e.g. `c6i.xlarge`/`c7i.xlarge`).
- For lower ASR latency, a small GPU instance (`g4dn.xlarge`, NVIDIA T4) gives comfortable
  headroom for real-time `faster-whisper` plus Piper TTS alongside.
- Security group: only allow inbound WebSocket from your device's network during the demo.

### 7.2 ASR + end-pointing

- Server-side Silero VAD on the incoming (Opus-decoded) PCM stream decides when the user has
  stopped talking (~700 ms–1 s of silence) — this is the authoritative signal that ends
  `STREAMING_TO_ASR` on the device.
- Run `faster-whisper` on the accumulated utterance once end-of-speech fires.

### 7.3 MCP client (server-side)

The cloud server acts as an **MCP client** to the device's MCP server:

1. During WebSocket handshake, server sends `mcp:initialize` → device responds with
   capabilities and tool list.
2. Server caches the device's tool list and includes it in the LLM system prompt as
   available functions/tools.
3. When the LLM generates a tool call (e.g. `self.lamp.turn_on`), the server forwards it
   as a `mcp:tools/call` JSON message over the WebSocket to the device.
4. Device executes the tool, returns the result. Server feeds the result back to the LLM
   for the next response turn.
5. Multi-turn tool use is supported: the LLM can chain multiple tool calls in one
   conversation turn (e.g. read sensor → decide → control actuator).

This is the architecture that makes "Hey Voxie, if the temperature is above 30 degrees,
turn on the fan and set the LED strip to red" work as a single natural-language command.

### 7.4 LLM reply

Write a provider-agnostic `llm.py` with one function,
`generate_reply(transcript, history, tools) -> (str, list[tool_calls])`, and a thin adapter
per provider (OpenAI/Anthropic/Gemini) selected by an environment variable. The `tools`
parameter includes the device's MCP tool schemas so the LLM can generate structured tool
calls alongside natural language replies.

### 7.5 TTS

Piper TTS for fast, local synthesis on the same server. **Critical change from beta plan**:
encode Piper's output into Opus before streaming back over the WebSocket. This matches the
device's decoder and keeps bandwidth low in both directions.

Stream Opus frames back in the same small-chunk framing so playback starts before the full
reply has finished synthesizing.

### 7.6 Vision pipeline (stretch)

When the device's `self.camera.take_photo` MCP tool is called:
1. Device captures JPEG from camera, uploads via HTTP multipart POST to the server's
   `/vision/upload` endpoint.
2. Server forwards the image + the user's question to a vision-capable LLM (GPT-4V,
   Claude, Gemini Pro Vision).
3. Returns the textual description to the MCP tool result.
4. LLM incorporates the description into its spoken reply.

### 7.7 Async notifications

The server can push notifications to idle devices:

```json
{
  "type": "notify",
  "text": "Your timer is done!",
  "audio_url": "https://server/notifications/timer_done.opus"
}
```

Device transitions to `NOTIFYING` state, plays the audio with synchronized text on the
display, then returns to `IDLE_LISTENING`. Wake word cancels notification playback.

### 7.8 Logging

Log a timestamp at every stage boundary (audio received, Opus decoded, end-of-speech
detected, transcription complete, LLM reply received, MCP tool calls, first TTS byte sent)
for every session. This is what §10's latency report is built from.

---

## 8. Network protocol

### 8.1 Connection model

One persistent WebSocket connection per device session (established once Wi-Fi is up, kept
alive with pings), carrying both directions — Opus audio up, Opus audio + JSON control
down — distinguished by text frames (JSON) and binary frames (Opus audio with optional
packed header).

### 8.2 Binary frame format

Adopt XiaoZhi's packed binary header for audio frames (timestamps enable server-side latency
measurement and potential AEC alignment):

```c
struct __attribute__((packed)) BinaryFrameHeader {
    uint16_t version;       // protocol version (1)
    uint16_t type;          // 0=opus_mic_audio, 1=opus_tts_audio
    uint32_t timestamp_ms;  // device uptime or SNTP-synced epoch ms
    uint32_t payload_size;  // size of the Opus frame following this header
};
// Total header: 12 bytes, followed by Opus frame payload
```

### 8.3 JSON message framing

```
Upstream (device → server):
  {"type":"hello","device_id":"<MAC>","firmware_version":"1.0.0",
   "features":{"mcp":true,"opus":true,"vision":false},
   "sample_rate":16000,"opus_frame_ms":60}

  {"type":"stream_start","wake_ts_us":<uint64>,"mode":"auto"}
  <binary frames: Opus-encoded mic audio>  (repeated)
  {"type":"stream_end"}

  {"type":"abort","reason":"wake_word_detected"}

  {"type":"mcp","id":<int>,"result":{...}}          // MCP response from device

Downstream (server → device):
  {"type":"hello","session_id":"<uuid>",
   "features":{"mcp":true,"opus":true,"vision_url":"https://..."}}

  {"type":"stt","text":"what's the temperature"}     // live transcript for display
  {"type":"tts","state":"start","text":"It's 28°C"}
  <binary frames: Opus-encoded TTS audio>  (repeated)
  {"type":"tts","state":"end"}

  {"type":"llm","emotion":"happy"}                   // drive display animations

  {"type":"mcp","id":<int>,"method":"tools/call",    // MCP request to device
   "params":{"name":"self.lamp.turn_on","arguments":{}}}

  {"type":"notify","text":"Timer done!","audio_url":"..."}
```

### 8.4 Latency instrumentation (this is the graded number)

1. On boot, and periodically, sync the device's clock via SNTP.
2. `wake_ts_us` in the `stream_start` header is the device's timestamp (synced epoch, not
   `esp_timer` uptime) for the **end of the detected keyword** — i.e. the pre-roll buffer
   position where the wake word finished, not the moment the detection task got around to
   firing.
3. Server records its own wall-clock receipt time for that same `stream_start` frame.
4. `latency = server_recv_ts - wake_ts_us`, corrected for any residual SNTP clock offset
   (log the offset from your SNTP sync alongside each measurement so you can report an
   error bound, not just a point estimate).

---

## 9. Phase 5 — IoT demo scenarios

These are the scenarios that make the demo unforgettable. Each requires only a registered
MCP tool + trivial hardware (a relay, a sensor, an LED strip):

### 9.1 Smart lamp control

```
User: "Hey Voxie, turn on the desk lamp"
Flow: KWS → ASR("turn on the desk lamp") → LLM decides to call self.lamp.turn_on
      → MCP tools/call → device toggles GPIO → relay clicks → lamp turns on
      → LLM speaks "Done, the lamp is on"
```

### 9.2 Sensor query

```
User: "Hey Voxie, what's the temperature in here?"
Flow: KWS → ASR → LLM calls self.sensor.read_temperature
      → MCP tools/call → device reads DHT22 → returns "28.5°C"
      → LLM speaks "It's 28 and a half degrees"
```

### 9.3 Multi-step reasoning

```
User: "Hey Voxie, if it's warmer than 25 degrees, turn on the fan and make the LEDs blue"
Flow: KWS → ASR → LLM calls self.sensor.read_temperature → gets "28.5°C"
      → LLM reasons: 28.5 > 25, so calls self.lamp.turn_on (fan on relay)
      → then calls self.led_strip.set_color(r=0,g=0,b=255)
      → speaks "It's 28.5 degrees, I've turned on the fan and set the LEDs to blue"
```

### 9.4 Device self-awareness

```
User: "Hey Voxie, how are you doing?"
Flow: LLM calls self.get_device_status → gets {volume:70, battery:85, rssi:-42, heap:120000}
      → speaks "I'm doing great! Battery at 85%, strong Wi-Fi signal, and plenty of memory"
```

### 9.5 Theme control

```
User: "Hey Voxie, switch to dark mode"
Flow: LLM calls self.screen.set_theme("dark") → LVGL theme changes live
      → speaks "Switched to dark mode"
```

### 9.6 Camera vision (stretch)

```
User: "Hey Voxie, what's written on this whiteboard?"
Flow: LLM calls self.camera.take_photo(question="what's written on this whiteboard?")
      → device captures JPEG → uploads to server → vision LLM analyzes
      → speaks "I can see a list of three items: buy groceries, finish the report..."
```

---

## 10. Testing & evaluation protocol

Run these and write the results into `/docs/latency_report.md` — this is your evidence for
the judged metrics.

**Efficiency:**
- Flash/RAM footprint: `idf.py size-components` for flash; `heap_caps_get_free_size()` before
  wake-word activity starts, and the tensor arena size actually used by
  `AllocateTensors()`, for RAM.
- Idle CPU: `vTaskGetRunTimeStats()` sampled over a multi-minute idle-listening window with
  no wake word spoken. The Opus encoder/decoder should contribute **zero** CPU while idle
  since they only run during active streaming.

**Accuracy:**
- False-reject rate: play back your held-out positive test set (real recordings, not just
  synthetic) through the actual mic/speaker setup you'll demo with, count misses.
- False-accept rate: run several hours of ambient audio (podcasts, conversation, TV, whatever
  the demo hall sounds like) through the live device and count triggers. Report both rates
  together — tuning one without the other is meaningless.

**Latency:**
- Run the §8.4 measurement across at least 20–30 trials, at both the shortest and longest
  wake-word phrasing variations you expect, and report mean + spread.

**Demo-layer metrics** (not judged, but good to have):
- End-to-end time from wake-word to first TTS audio byte played on speaker.
- MCP tool call round-trip time (WS → device → execute → WS → LLM).
- Opus encoding CPU overhead on ESP32-S3 (should be <5% at complexity 1, 60ms frames).

---

## 11. Build phases & priority order

| Phase | Priority | Description | Time estimate |
|---|---|---|---|
| **P0: KWS training** | Critical (judged) | Dataset generation, model training, evaluation, export | 2–3 days |
| **P1: Core firmware** | Critical (judged) | I2S mic, feature extraction, TFLite inference, basic state machine, Wi-Fi, raw WebSocket streaming | 3–4 days |
| **P2: Opus integration** | High | Add libopus encode/decode on device + server. Replace raw PCM streaming. | 1–2 days |
| **P3: Cloud backend** | High | FastAPI WS server, Silero VAD, faster-whisper ASR, LLM client, Piper TTS, Opus encode/decode | 2–3 days |
| **P4: MCP server** | High (demo impact) | JSON-RPC 2.0 on device, built-in tools, server-side MCP client, LLM tool-use integration | 2–3 days |
| **P5: Display** | Medium | LVGL LCD driver, chat bubble UI, status bar, themes, state animations. Or SSD1306 OLED fallback. | 2 days |
| **P6: IoT tools** | Medium (demo impact) | Lamp relay, sensor reader, LED strip — registered as MCP tools | 1 day |
| **P7: Wi-Fi provisioning** | Medium | SoftAP captive portal for credential entry | 1 day |
| **P8: OTA** | Low | Dual-partition OTA with rollback | 1 day |
| **P9: Async notifications** | Low | Cloud push notifications with audio playback | 0.5 day |
| **P10: Camera/Vision** | Stretch | Camera capture + vision LLM analysis via MCP | 1–2 days |
| **P11: Polish & testing** | Critical | Latency measurement, accuracy evaluation, demo rehearsal | 2 days |

**Minimum viable demo**: P0 + P1 + P3 (satisfies all judging criteria).
**Impressive demo**: P0–P6 (adds Opus, MCP, display, IoT — miles ahead of competition).
**Full vision**: P0–P11 (everything, including camera vision).

---

## 12. Demo-day checklist

- [ ] Confirm venue Wi-Fi reliability; have a laptop hotspot as backup.
- [ ] Pre-warm the server: models loaded, Piper/faster-whisper ready, no cold-start delays.
- [ ] Have the §10 numbers ready on a slide/printout — don't make judges take your word for it.
- [ ] Have a two-sentence answer for "why not just use ESP-SR/Picovoice" — point at §0.2.
- [ ] Prepare a scripted demo flow:
  1. Say wake word → ask a question → get spoken reply (basic pipeline).
  2. "Turn on the lamp" → relay clicks, lamp lights up (IoT via MCP).
  3. "What's the temperature?" → sensor reading spoken back (sensor via MCP).
  4. "Switch to dark mode" → LCD theme changes live (device control via MCP).
  5. "Set the LEDs to red" → LED strip changes (visual flair via MCP).
  6. (If camera) "What's on this paper?" → vision analysis spoken back.
- [ ] Test barge-in: interrupt assistant mid-sentence with the wake word.
- [ ] Have the MCP tools reference doc ready — judges will ask "what else can it do?"
- [ ] Prepare a fallback: if Wi-Fi dies, the wake-word detection still works locally and
      you can demo the KWS accuracy/latency numbers offline. The judged pipeline doesn't
      need the cloud to prove efficiency.

---

## Appendix A: Reference implementations to study

- `espressif/esp-tflite-micro` — official TFLite Micro port for ESP-IDF, includes the
  `micro_speech` example (KWS skeleton + audio front end) and ESP-NN acceleration.
  https://github.com/espressif/esp-tflite-micro
- `espressif/esp-nn` — the accelerated kernel library.
  https://github.com/espressif/esp-nn
- `OHF-Voice/micro-wake-word` (formerly `kahrendt/microWakeWord`) — the training framework.
  https://github.com/OHF-Voice/micro-wake-word
- `esphome/esphome`, path `esphome/components/micro_wake_word/` — reference C++ for the
  streaming inference/feature-extraction integration.
- `OHF-Voice/piper1-gpl` — TTS engine.
  https://github.com/OHF-Voice/piper1-gpl
- `SYSTRAN/faster-whisper` — ASR engine.
  https://github.com/SYSTRAN/faster-whisper
- `collabora/WhisperLive` — reference real-time whisper WebSocket server.
- `xiaozhi-esp32` (X:\Voxie\xiaozhi) — reference for MCP server implementation on ESP32-S3,
  Opus integration, LVGL display, IoT tool registration patterns, binary protocol framing.
- `xiph/opus` — Opus codec reference implementation.
  https://github.com/xiph/opus
- `snakers4/silero-vad` — Silero VAD for server-side end-pointing.
  https://github.com/snakers4/silero-vad
- `lvgl/lvgl` — LVGL graphics library (v9.x) for LCD UI.
  https://github.com/lvgl/lvgl

## Appendix B: MCP specification reference

MCP (Model Context Protocol) specification version 2024-11-05:
https://spec.modelcontextprotocol.io/specification/2024-11-05/

Key concepts implemented in Voxie:
- **JSON-RPC 2.0**: All MCP messages use standard JSON-RPC request/response format.
- **Tool Discovery**: `tools/list` returns all registered tools with JSON Schema input
  definitions, supporting cursor-based pagination.
- **Tool Invocation**: `tools/call` with `name` and `arguments`, returns `content` array
  with `text` or `image` items.
- **Capability Negotiation**: `initialize` exchange at session start establishes protocol
  version and supported features.

## Appendix C: Key architectural decisions vs. XiaoZhi

| Decision | XiaoZhi's approach | Voxie's approach | Rationale |
|---|---|---|---|
| Wake word engine | ESP-SR WakeNet (proprietary pre-trained) | microWakeWord + TFLite Micro (open-source, custom-trained) | SIH compliance: must be open-source, custom phrase |
| Audio codec | Opus (mandatory) | Opus (adopted) | 10× bandwidth savings, better reliability |
| Transport | WebSocket + MQTT/UDP hybrid | WebSocket only | Simpler, sufficient for Wi-Fi demo |
| MCP server | Full implementation | Full implementation (adopted) | Killer feature, extensible, standard protocol |
| Display | LVGL with 17+ LCD drivers | LVGL with ST7789 + SSD1306 fallback | We target one board, not 138 |
| Board abstraction | 138 boards, massive HAL | Single board config, minimal HAL | Premature abstraction wastes time for a competition |
| Multi-language | 40 locales | English only (add more if time permits) | Demo is in English |
| Glyph push | Dynamic font streaming | Baked font with good coverage | Simpler, reliable, sufficient |
| Network types | Wi-Fi, 4G, Ethernet, USB | Wi-Fi only | Demo venue has Wi-Fi |
| AEC | ESP-SR AFE (proprietary) | Trained-model resilience + speaker ducking | Keeps KWS fully open-source |
