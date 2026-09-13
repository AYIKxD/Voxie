#include "audio_service.h"
#include "system/config.h"
#include "esp_log.h"
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
static int s_volume = 70;  // 0–100

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

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
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
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_mic_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_mic_chan));
    ESP_LOGI(TAG, "I2S mic initialized (I2S%d, %d Hz)", MIC_I2S_NUM, MIC_SAMPLE_RATE);
}

static void init_i2s_speaker(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = DMA_BUF_LEN_SAMPLES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_spk_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
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
    ESP_LOGI(TAG, "I2S speaker initialized (I2S%d)", SPK_I2S_NUM);
}

// ============================================================================
// FreeRTOS tasks
// ============================================================================

/// Mic capture task — reads I2S DMA and pushes raw PCM into the mic ring buffer.
/// Pinned to Core 0 for deterministic timing alongside KWS.
static void i2s_mic_task(void *arg) {
    int16_t dma_buf[DMA_BUF_LEN_SAMPLES];
    size_t bytes_read = 0;

    ESP_LOGI(TAG, "Mic capture task started on core %d", xPortGetCoreID());

    while (1) {
        esp_err_t ret = i2s_channel_read(s_mic_chan, dma_buf, DMA_BUF_LEN_BYTES,
                                          &bytes_read, pdMS_TO_TICKS(100));
        if (ret == ESP_OK && bytes_read > 0) {
            // Push into ring buffer; drop if full (non-blocking to avoid stalling DMA)
            xRingbufferSend(s_mic_ringbuf, dma_buf, bytes_read, 0);
        }
    }
}

/// Playback task — pulls PCM from the playback ring buffer and writes to I2S speaker.
static void playback_task(void *arg) {
    int16_t dma_buf[DMA_BUF_LEN_SAMPLES];
    size_t bytes_written = 0;

    ESP_LOGI(TAG, "Playback task started on core %d", xPortGetCoreID());

    while (1) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_playback_ringbuf, &item_size,
                                             pdMS_TO_TICKS(50), DMA_BUF_LEN_BYTES);
        if (item != NULL && item_size > 0) {
            // Apply software volume scaling
            int16_t *samples = (int16_t *)item;
            size_t num_samples = item_size / sizeof(int16_t);
            float gain = (float)s_volume / 100.0f;
            for (size_t i = 0; i < num_samples; i++) {
                dma_buf[i] = (int16_t)((float)samples[i] * gain);
            }
            vRingbufferReturnItem(s_playback_ringbuf, item);

            i2s_channel_write(s_spk_chan, dma_buf, item_size, &bytes_written,
                              pdMS_TO_TICKS(100));
        } else {
            // No audio to play — write silence to keep DMA happy
            memset(dma_buf, 0, DMA_BUF_LEN_BYTES);
            i2s_channel_write(s_spk_chan, dma_buf, DMA_BUF_LEN_BYTES, &bytes_written,
                              pdMS_TO_TICKS(50));
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void audio_service_init(void) {
    // Create ring buffers
    // Mic ring buffer: ~2 seconds of 16kHz mono 16-bit audio
    s_mic_ringbuf = xRingbufferCreate(AUDIO_RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    configASSERT(s_mic_ringbuf != NULL);

    // Playback ring buffer: same size
    s_playback_ringbuf = xRingbufferCreate(AUDIO_RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    configASSERT(s_playback_ringbuf != NULL);

    // Initialize I2S hardware
    init_i2s_mic();
    init_i2s_speaker();

    ESP_LOGI(TAG, "Audio service initialized (ring buf: %d bytes each)", AUDIO_RING_BUFFER_SIZE);
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
}

int audio_service_get_volume(void) {
    return s_volume;
}
