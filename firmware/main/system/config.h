#pragma once

#include "driver/gpio.h"

// ============================================================================
// Voxie Configuration — All compile-time constants and feature flags
// ============================================================================

// --- Wake Word ---
#define VOXIE_WAKE_WORD          "Hey Voxie"

// --- Feature Flags (set via menuconfig or sdkconfig.defaults) ---
#ifndef VOXIE_ENABLE_MCP
#define VOXIE_ENABLE_MCP         1
#endif

#ifndef VOXIE_ENABLE_IOT
#define VOXIE_ENABLE_IOT         1
#endif

#ifndef VOXIE_ENABLE_CAMERA
#define VOXIE_ENABLE_CAMERA      0
#endif

#ifndef VOXIE_ENABLE_OTA
#define VOXIE_ENABLE_OTA         1
#endif

#ifndef VOXIE_ENABLE_LCD
#define VOXIE_ENABLE_LCD         0   // 0 = SSD1306 OLED (current wiring), 1 = ST7789 LCD
#endif

#ifndef VOXIE_ENABLE_LED_STRIP
#define VOXIE_ENABLE_LED_STRIP   1
#endif

// --- ML / codec backends (provided by managed components) ---
// TFLite Micro is used by the KWS engine for on-device wake-word inference.
#ifndef VOXIE_HAS_TFLITE
#define VOXIE_HAS_TFLITE         1
#endif

// --- I2S Mic (RX) - I2S_NUM_0 ---
// ICS43434 I2S MEMS mic. Pinout is identical to the INMP441 (WS/SCK/SD/L-R),
// so no wiring or firmware change is required to swap between them.
// Tie L/R (select) to GND for left-channel/mono capture.
#define MIC_I2S_NUM              I2S_NUM_0
#define MIC_I2S_WS               GPIO_NUM_4
#define MIC_I2S_SCK              GPIO_NUM_5
#define MIC_I2S_SD               GPIO_NUM_6
#define MIC_SAMPLE_RATE          16000
#define MIC_BITS_PER_SAMPLE      16

// --- I2S Speaker (TX) - I2S_NUM_1 ---
// MAX98357A: WS/LRC, SCK/BCLK, SD/DIN (match the actual wiring).
#define SPK_I2S_NUM              I2S_NUM_1
#define SPK_I2S_WS               GPIO_NUM_7    // MAX98357A LRC
#define SPK_I2S_SCK              GPIO_NUM_15   // MAX98357A BCLK
#define SPK_I2S_SD               GPIO_NUM_16   // MAX98357A DIN

// --- SPI LCD (ST7789) ---
#define LCD_SPI_HOST             SPI2_HOST
#define LCD_PIN_MOSI             GPIO_NUM_11
#define LCD_PIN_SCLK             GPIO_NUM_12
#define LCD_PIN_CS               GPIO_NUM_10
#define LCD_PIN_DC               GPIO_NUM_13
#define LCD_PIN_RST              GPIO_NUM_14
#define LCD_PIN_BL               GPIO_NUM_21
#define LCD_WIDTH                240
#define LCD_HEIGHT               280

// --- I2C OLED (SSD1306) ---
#define OLED_I2C_SDA             GPIO_NUM_41
#define OLED_I2C_SCL             GPIO_NUM_42
#define OLED_WIDTH               128
#define OLED_HEIGHT              64

// --- WS2812B LED Strip ---
#define LED_STRIP_GPIO           GPIO_NUM_48
#define LED_STRIP_NUM_LEDS       16

// --- IoT GPIOs ---
#define LAMP_RELAY_GPIO          GPIO_NUM_47
#define SENSOR_DHT_GPIO          GPIO_NUM_46
#define BUTTON_GPIO              GPIO_NUM_0   // BOOT button

// --- Audio / Opus ---
#define OPUS_BITRATE             16000
#define OPUS_FRAME_DURATION_MS   60
#define OPUS_COMPLEXITY           1
#define AUDIO_RING_BUFFER_SIZE   (MIC_SAMPLE_RATE * 2 * 2)  // 2 seconds of 16-bit mono PCM
#define PRE_ROLL_BUFFER_MS       800

// --- KWS ---
#define KWS_FEATURE_STRIDE_MS    10
#define KWS_FEATURE_SIZE         40
#define KWS_STREAMING_SLICES     3    // microWakeWord streaming input window
#define KWS_AVG_WINDOW           5    // moving-average window over frame probabilities
#define KWS_DETECTION_THRESHOLD  0.50f
#define KWS_SMOOTHING_WINDOW     3    // consecutive averaged frames above threshold to trigger

// --- Network ---
#define WS_RECONNECT_INTERVAL_MS 5000
#define SNTP_SERVER              "pool.ntp.org"
#define SOFTAP_SSID_PREFIX       "Voxie"

// --- Task stack sizes ---
#define STACK_I2S_MIC            4096
#define STACK_FEATURE            4096
#define STACK_KWS                8192
#define STACK_OPUS_ENCODE        8192
#define STACK_STREAM             8192
#define STACK_OPUS_DECODE        8192
#define STACK_PLAYBACK           4096
#define STACK_MCP                8192
#define STACK_DISPLAY            8192
#define STACK_LED                2048
