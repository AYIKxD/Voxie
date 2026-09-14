# Voxie — Low-Latency Voice Activator for Edge Devices

> **SIH (Smart India Hackathon) Project** — Custom wake-word recognition on ESP32-S3 with cloud-backed conversational AI and MCP-powered IoT control.

##  Key Features

| Feature | Description |
|---------|-------------|
|  **Custom Wake Word** | Open-source KWS trained on your own data (microWakeWord + TFLite Micro) |
|  **MCP Server on MCU** | LLM dynamically discovers & calls device tools via JSON-RPC 2.0 |
|  **Barge-in** | Interrupt the assistant mid-sentence with a new command |
|  **IoT Control** | Natural language control of lamps, sensors, LED strips |
|  **LVGL Display** | Chat bubbles, dark/light themes, status bar on ST7789 LCD |
|  **LED Animations** | State-driven WS2812B effects (breathe, chase, rainbow) |
|  **Wi-Fi Provisioning** | Phone-friendly captive portal, no hardcoded credentials |
|  **OTA Updates** | Dual-partition firmware updates with automatic rollback |
|  **Async Notifications** | Cloud pushes spoken alerts to idle devices |
|  **Sub-500ms Latency** | SNTP-synced end-to-end latency measurement |

##  Architecture

```
┌──────────────────── ESP32-S3 Device ────────────────────┐
│  I2S Mic → KWS (TFLite) → Opus Encoder → WebSocket →   │
│  ← Opus Decoder ← WebSocket ← I2S Speaker              │
│  MCP Server ← JSON-RPC ← Cloud MCP Client              │
│  LVGL LCD | WS2812B LEDs | GPIO Relay | DHT22           │
└─────────────────────────────────────────────────────────┘
                          ↕ WebSocket
┌──────────────────── Cloud Server ───────────────────────┐
│  FastAPI + faster-whisper ASR + Piper TTS + LLM         │
│  MCP Client → tool calls → device                      │
└─────────────────────────────────────────────────────────┘
```

##  Project Structure

```
Voxie/
├── firmware/               # ESP-IDF project
│   ├── main/
│   │   ├── app_main.cpp    # Entry point, subsystem wiring
│   │   ├── audio/          # I2S mic capture + speaker playback
│   │   ├── kws/            # Wake-word engine (TFLite Micro)
│   │   ├── protocol/       # WebSocket + binary frame protocol
│   │   ├── mcp/            # MCP server + built-in tools
│   │   ├── display/        # LVGL LCD + OLED + LED strip
│   │   ├── iot/            # IoT tools (lamp, sensor, LEDs)
│   │   ├── notify/         # Async notification player
│   │   └── system/         # Config, state machine, Wi-Fi, OTA
│   ├── partitions/         # Dual-OTA partition table
│   ├── CMakeLists.txt
│   └── sdkconfig.defaults
├── server/                 # Cloud backend (Python)
│   ├── app.py              # FastAPI WebSocket server
│   ├── asr.py              # faster-whisper ASR
│   ├── llm.py              # Pluggable LLM (OpenAI/Anthropic/Gemini)
│   ├── tts.py              # Piper TTS
│   ├── mcp_client.py       # MCP client (forwards tool calls to device)
│   ├── opus_utils.py       # Opus encode/decode
│   ├── vad.py              # Silero VAD
│   ├── Dockerfile
│   └── docker-compose.yml
├── training/               # Wake-word model training
│   ├── generate_dataset.py # Synthetic data generation (Piper TTS)
│   ├── augment.py          # Audio augmentation (noise, RIR, speed)
│   ├── train.py            # microWakeWord training wrapper
│   ├── export_tflite.py    # Int8 quantized TFLite export
│   └── evaluate.py         # FAR/FRR accuracy evaluation
└── build.md                # Complete build plan & specification
```

##  Quick Start

### Prerequisites
- ESP-IDF v5.x installed ([guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/))
- Python 3.10+
- Docker (for server)

### Build Firmware
```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

### Start Server
```bash
cd server
docker-compose up --build
```

### Train Wake Word
```bash
cd training
pip install -r requirements.txt  # (create venv first)
python generate_dataset.py --wake-word "Hey Voxie"
python augment.py --input-dir data/ --output-dir data_augmented/
python train.py --data-dir data_augmented/
python export_tflite.py --model-dir output/ --output ../firmware/main/kws/model_data.tflite
```

##  Hardware Requirements

| Component | Required | Purpose |
|-----------|----------|---------|
| ESP32-S3-DevKitC-1 (8MB flash, PSRAM) | ✅ | Main MCU |
| ICS43434 I2S MEMS Mic (INMP441-compatible) | ✅ | Voice capture |
| MAX98357A I2S Amplifier + Speaker | ✅ | Audio playback |
| ST7789 1.69" LCD (240×280) | Optional | Rich UI display |
| SSD1306 0.96" OLED | Optional | Basic status display |
| WS2812B LED Strip (16 LEDs) | Optional | State animations |
| Relay Module | Optional | Lamp control demo |
| DHT22 Sensor | Optional | Temperature/humidity demo |

##  SIH Differentiators

1. **Open-source everything** — no proprietary wake-word engine (unlike ESP-SR/WakeNet)
2. **Custom-trained KWS** — judges can verify the entire training pipeline
3. **MCP on microcontroller** — first-of-its-kind embedded MCP server
4. **Measurable latency** — SNTP-synced timestamps prove sub-500ms wake-to-action

##  License

MIT
