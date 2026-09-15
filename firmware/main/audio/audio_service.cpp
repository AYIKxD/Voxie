#include "audio_service.h"
#include "system/config.h"
#include "system/settings.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>

static const char *TAG = "audio_svc";

// --- I2S channel handles ---
static i2s_chan_handle_t s_mic_chan = NULL;
static i2s_chan_handle_t s_spk_chan = NULL;

// --- Ring buffers ---
static RingbufHandle_t s_mic_ringbuf = NULL;
static RingbufHandle_t s_playback_ringbuf = NULL;

// --- Volume (software gain) ---
#define VOLUME_DEFAULT 70
#define VOLUME_SETTINGS_NS "voxie_audio"
static int s_volume = VOLUME_DEFAULT;  // 0â€“100

// --- DMA buffer for I2S reads/writes ---
#define DMA_BUF_LEN_SAMPLES  512
#define DMA_BUF_LEN_BYTES    (DMA_BUF_LEN_SAMPLES * sizeof(int16_t))

// ============================================================================
// I2S initialization
// ============================================================================

static void init_i2s_mic(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = DMA_BUF_LEN_SAMPLES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_mic_chan));

    // ICS43434 is a 24-bit mic: capture 32-bit slots (data left-justified) and
    // select the left channel. These settings match the esp-tflite-micro
    // micro_speech reference, which is tested with the ICS43434.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_SCK,
            .ws   = MIC_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_I2S_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_mic_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_mic_chan));
    ESP_LOGI(TAG, "I2S mic initialized (I2S%d, %d Hz, 32-bit slots)", MIC_I2S_NUM, MIC_SAMPLE_RATE);
}

static void init_i2s_speaker(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = DMA_BUF_LEN_SAMPLES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_spk_chan, NULL));

    // MAX98357A is a 16-bit I2S DAC and expects a standard stereo frame
    // (L+R per LRCK). Running at 24kHz gives FM-radio quality TTS (max 12kHz)
    // vs 16kHz telephone quality. The 32-bit mono experiment distorted badly.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_I2S_SCK,
            .ws   = SPK_I2S_WS,
            .dout = SPK_I2S_SD,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_spk_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_spk_chan));
    ESP_LOGI(TAG, "I2S speaker initialized (I2S%d, %d Hz, 16-bit stereo)", SPK_I2S_NUM, SPK_SAMPLE_RATE);
}

// ============================================================================
// FreeRTOS tasks
// ============================================================================

/// Mic capture task â€” reads I2S DMA and pushes raw PCM into the mic ring buffer.
/// Pinned to Core 0 for deterministic timing alongside KWS.
static void i2s_mic_task(void *arg) {
    // Static buffers: these are too large to keep on the task stack.
    static int32_t raw_buf[DMA_BUF_LEN_SAMPLES];
    static int16_t pcm_buf[DMA_BUF_LEN_SAMPLES];
    size_t bytes_read = 0;

    ESP_LOGI(TAG, "Mic capture task started on core %d", xPortGetCoreID());

    while (1) {
        esp_err_t ret = i2s_channel_read(s_mic_chan, raw_buf, sizeof(raw_buf),
                                          &bytes_read, pdMS_TO_TICKS(100));
        if (ret == ESP_OK && bytes_read > 0) {
            size_t n = bytes_read / sizeof(int32_t);
            // 24-bit sample is left-justified in the 32-bit slot: keep the
            // top 16 bits to produce a 16-bit PCM sample.
            for (size_t i = 0; i < n; i++) {
                pcm_buf[i] = (int16_t)(raw_buf[i] >> 16);
            }
            // Push into ring buffer; drop if full (non-blocking to avoid stalling DMA)
            xRingbufferSend(s_mic_ringbuf, pcm_buf, n * sizeof(int16_t), 0);
        }
    }
}

/// Playback task â€” pulls PCM from the playback ring buffer and writes to I2S speaker.
static void playback_task(void *arg) {
    // Static buffers: these are too large to keep on the task stack.
    static int16_t dma_buf[DMA_BUF_LEN_SAMPLES];
    static int16_t stereo_buf[DMA_BUF_LEN_SAMPLES * 2];
    size_t bytes_written = 0;

    ESP_LOGI(TAG, "Playback task started on core %d", xPortGetCoreID());

    while (1) {
        size_t item_size = 0;
        // Don't wait long. If we don't have data, we must feed silence to I2S DMA.
        void *item = xRingbufferReceiveUpTo(s_playback_ringbuf, &item_size,
                                             pdMS_TO_TICKS(10), DMA_BUF_LEN_BYTES);
        size_t num_samples = 0;

        if (item != NULL && item_size > 0) {
            int16_t *samples = (int16_t *)item;
            num_samples = item_size / sizeof(int16_t);
            
            // Exponential volume curve in Q16 fixed point (no float per sample).
            double vol_normalized = (double)s_volume / 100.0;
            int32_t volume_factor = (int32_t)(vol_normalized * vol_normalized * 65536.0);

            for (size_t i = 0; i < num_samples; i++) {
                int32_t scaled = (int32_t)(((int32_t)samples[i] * volume_factor) >> 16);
                if (scaled > INT16_MAX) scaled = INT16_MAX;
                else if (scaled < INT16_MIN) scaled = INT16_MIN;
                dma_buf[i] = (int16_t)scaled;
            }
            vRingbufferReturnItem(s_playback_ringbuf, item);
        } else {
            // No audio to play â€” emit silence to keep DMA/amp running cleanly
            memset(dma_buf, 0, sizeof(dma_buf));
            num_samples = DMA_BUF_LEN_SAMPLES;
        }

        // MAX98357A needs a standard stereo frame: duplicate mono to L/R.
        for (size_t i = 0; i < num_samples; i++) {
            stereo_buf[2 * i]     = dma_buf[i];
            stereo_buf[2 * i + 1] = dma_buf[i];
        }

        i2s_channel_write(s_spk_chan, stereo_buf,
                          num_samples * 2 * sizeof(int16_t),
                          &bytes_written, pdMS_TO_TICKS(100));
    }
}

// ============================================================================
// Public API
// ============================================================================

static RingbufHandle_t create_audio_ringbuf(size_t size) {
    // Prefer PSRAM to keep internal RAM free for the Wi-Fi driver/DMA.
    RingbufHandle_t h = xRingbufferCreateWithCaps(
        size, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (h == NULL) {
        h = xRingbufferCreate(size, RINGBUF_TYPE_BYTEBUF);
    }
    return h;
}

void audio_service_init(void) {
    // Create ring buffers
    // Mic ring buffer: ~2 seconds of 16kHz mono 16-bit audio
    s_mic_ringbuf = create_audio_ringbuf(AUDIO_RING_BUFFER_SIZE);
    configASSERT(s_mic_ringbuf != NULL);

    // Playback ring buffer: same size
    s_playback_ringbuf = create_audio_ringbuf(AUDIO_RING_BUFFER_SIZE);
    configASSERT(s_playback_ringbuf != NULL);

    // Restore the last user-selected volume (persisted in NVS)
    if (settings_open(VOLUME_SETTINGS_NS, false)) {
        s_volume = settings_get_int("volume", VOLUME_DEFAULT);
        settings_close();
    }

    // Initialize I2S hardware
    init_i2s_mic();
    init_i2s_speaker();

    ESP_LOGI(TAG, "Audio service initialized (ring buf: %d bytes each, volume %d%%)",
             AUDIO_RING_BUFFER_SIZE, s_volume);
}

void audio_service_start(void) {
    // Mic task on Core 0 (audio-critical core)
    xTaskCreatePinnedToCore(i2s_mic_task, "i2s_mic", STACK_I2S_MIC,
                            NULL, configMAX_PRIORITIES - 1, NULL, 0);

    // Playback task on Core 1
    xTaskCreatePinnedToCore(playback_task, "playback", STACK_PLAYBACK,
                            NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Audio tasks started");
}

size_t audio_service_read_mic(int16_t *buf, size_t num_samples, uint32_t timeout_ms) {
    size_t wanted_bytes = num_samples * sizeof(int16_t);
    size_t total_read = 0;

    while (total_read < wanted_bytes) {
        size_t item_size = 0;
        size_t remaining = wanted_bytes - total_read;
        void *item = xRingbufferReceiveUpTo(s_mic_ringbuf, &item_size,
                                             pdMS_TO_TICKS(timeout_ms), remaining);
        if (item == NULL) break;

        memcpy((uint8_t *)buf + total_read, item, item_size);
        vRingbufferReturnItem(s_mic_ringbuf, item);
        total_read += item_size;
    }

    return total_read / sizeof(int16_t);
}

bool audio_service_write_playback(const int16_t *buf, size_t num_samples, uint32_t timeout_ms) {
    size_t bytes = num_samples * sizeof(int16_t);
    return xRingbufferSend(s_playback_ringbuf, buf, bytes, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void audio_service_flush_playback(void) {
    // Drain everything from the playback ring buffer
    size_t item_size;
    void *item;
    while ((item = xRingbufferReceive(s_playback_ringbuf, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(s_playback_ringbuf, item);
    }
    ESP_LOGI(TAG, "Playback buffer flushed (barge-in)");
}

RingbufHandle_t audio_service_get_mic_ringbuf(void) {
    return s_mic_ringbuf;
}

void audio_service_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    ESP_LOGI(TAG, "Volume set to %d%%", s_volume);

    // Persist so the choice survives reboots (xiaozhi keeps this in NVS too)
    if (settings_open(VOLUME_SETTINGS_NS, true)) {
        settings_set_int("volume", s_volume);
        settings_close();
    }
}

int audio_service_get_volume(void) {
    return s_volume;
}
