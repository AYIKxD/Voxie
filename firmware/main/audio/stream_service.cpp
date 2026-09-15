#include "stream_service.h"
#include "system/config.h"
#include "system/state_machine.h"
#include "audio/audio_service.h"
#include "protocol/websocket_protocol.h"
#include "protocol/binary_protocol.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/queue.h"
#include "opus.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

static const char *TAG = "stream_svc";

#define MONITOR_RINGBUF_SIZE  (MIC_SAMPLE_RATE * 2)          // 1 s of 16-bit mono
#define PRE_ROLL_MS           800
#define PRE_ROLL_SAMPLES      (MIC_SAMPLE_RATE * PRE_ROLL_MS / 1000)
#define OPUS_FRAME_SAMPLES    (MIC_SAMPLE_RATE * OPUS_FRAME_DURATION_MS / 1000)  // 960 @ 16kHz mic
#define TTS_FRAME_SAMPLES     (SPK_SAMPLE_RATE * OPUS_FRAME_DURATION_MS / 1000)  // 1440 @ 24kHz spk
#define MAX_TTS_FRAME_SAMPLES (TTS_FRAME_SAMPLES * 2)
#define VAD_RMS_THRESHOLD     600.0f
#define VAD_SILENCE_END_MS    500
#define MAX_STREAM_MS         12000

typedef struct {
    size_t len;
    uint8_t data[];
} tts_item_t;

static RingbufHandle_t s_monitor = NULL;
static OpusEncoder *s_enc = NULL;
static OpusDecoder *s_dec = NULL;
static QueueHandle_t s_tts_queue = NULL;
static TaskHandle_t s_stream_task = NULL;
static TaskHandle_t s_decode_task = NULL;

static volatile bool s_active = false;
static volatile bool s_just_started = false;
static volatile bool s_flush_tts_req = false;
static bool s_speech_seen = false;
static uint32_t s_silence_ms = 0;
static int64_t s_stream_start_us = 0;

static int16_t s_pre_roll[PRE_ROLL_SAMPLES];
static size_t s_pre_roll_pos = 0;

static int16_t s_enc_acc[OPUS_FRAME_SAMPLES];
static size_t s_enc_fill = 0;

// ---------------------------------------------------------------------------
// Opus encode helpers (upstream path)
// ---------------------------------------------------------------------------

static void send_opus_frame(void) {
    if (s_enc == NULL) return;

    uint8_t payload[1500];
    int n = opus_encode(s_enc, s_enc_acc, OPUS_FRAME_SAMPLES, payload,
                        sizeof(payload));
    if (n <= 0) {
        ESP_LOGW(TAG, "opus_encode failed: %d", n);
        return;
    }

    uint8_t frame[BINARY_HEADER_SIZE + sizeof(payload)];
    binary_frame_header_t hdr;
    hdr.version = BINARY_PROTO_VERSION;
    hdr.type = BINARY_TYPE_MIC_AUDIO;
    hdr.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    hdr.payload_size = (uint32_t)n;
    memcpy(frame, &hdr, BINARY_HEADER_SIZE);
    memcpy(frame + BINARY_HEADER_SIZE, payload, n);

    ws_protocol_send_binary(frame, BINARY_HEADER_SIZE + n);
}

static void encode_acc_push(const int16_t *pcm, size_t count) {
    size_t off = 0;
    while (off < count) {
        size_t take = OPUS_FRAME_SAMPLES - s_enc_fill;
        if (take > count - off) take = count - off;
        memcpy(s_enc_acc + s_enc_fill, pcm + off, take * sizeof(int16_t));
        s_enc_fill += take;
        off += take;
        if (s_enc_fill == OPUS_FRAME_SAMPLES) {
            send_opus_frame();
            s_enc_fill = 0;
        }
    }
}

static void flush_pre_roll(void) {
    size_t start = s_pre_roll_pos % PRE_ROLL_SAMPLES;
    for (size_t i = 0; i < PRE_ROLL_SAMPLES; i++) {
        s_enc_acc[s_enc_fill++] = s_pre_roll[(start + i) % PRE_ROLL_SAMPLES];
        if (s_enc_fill == OPUS_FRAME_SAMPLES) {
            send_opus_frame();
            s_enc_fill = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Stream task (always running): feeds pre-roll and, when active, encodes
// ---------------------------------------------------------------------------

static void stream_task(void *arg) {
    int16_t chunk[160];

    while (1) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_monitor, &item_size,
                                            pdMS_TO_TICKS(20), sizeof(chunk));
        if (item == NULL) {
            continue;
        }

        size_t count = item_size / sizeof(int16_t);
        memcpy(chunk, item, item_size);
        vRingbufferReturnItem(s_monitor, item);

        // Maintain the pre-roll ring buffer.
        for (size_t i = 0; i < count; i++) {
            s_pre_roll[s_pre_roll_pos % PRE_ROLL_SAMPLES] = chunk[i];
            s_pre_roll_pos++;
        }

        if (!s_active) {
            continue;
        }

        if (s_just_started) {
            flush_pre_roll();
            s_just_started = false;
        } else {
            encode_acc_push(chunk, count);
        }

        // Simple energy VAD to end the utterance.
        float energy = 0.0f;
        for (size_t i = 0; i < count; i++) {
            float v = (float)chunk[i];
            energy += v * v;
        }
        float rms = sqrtf(energy / (count > 0 ? count : 1));
        if (rms > VAD_RMS_THRESHOLD) {
            s_speech_seen = true;
            s_silence_ms = 0;
        } else if (s_speech_seen) {
            s_silence_ms += (uint32_t)(count * 1000 / MIC_SAMPLE_RATE);
            if (s_silence_ms >= VAD_SILENCE_END_MS) {
                stream_service_stop();
            }
        }

        if ((esp_timer_get_time() - s_stream_start_us) / 1000 > MAX_STREAM_MS) {
            stream_service_stop();
        }
    }
}

// ---------------------------------------------------------------------------
// Decode task (downstream path): Opus TTS -> playback ring buffer
// ---------------------------------------------------------------------------

static void decode_task(void *arg) {
    int16_t pcm[MAX_TTS_FRAME_SAMPLES];

    while (1) {
        tts_item_t *it = NULL;
        if (xQueueReceive(s_tts_queue, &it, portMAX_DELAY) != pdTRUE || it == NULL) {
            continue;
        }
        if (s_dec != NULL && !s_flush_tts_req) {
            int samples = opus_decode(s_dec, it->data, (opus_int32)it->len, pcm,
                                      MAX_TTS_FRAME_SAMPLES, 0);
            if (samples > 0) {
                audio_service_write_playback(pcm, (size_t)samples, 200);
            } else {
                ESP_LOGW(TAG, "opus_decode failed: %d", samples);
            }
        }
        free(it);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void stream_service_init(void) {
    // Prefer PSRAM for the monitor buffer to preserve internal RAM.
    s_monitor = xRingbufferCreateWithCaps(
        MONITOR_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_monitor == NULL) {
        s_monitor = xRingbufferCreate(MONITOR_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    }

    int err = OPUS_OK;
    s_enc = opus_encoder_create(MIC_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || s_enc == NULL) {
        ESP_LOGE(TAG, "opus_encoder_create failed: %d", err);
        s_enc = NULL;
    } else {
        opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    }

    s_dec = opus_decoder_create(SPK_SAMPLE_RATE, 1, &err);
    if (err != OPUS_OK || s_dec == NULL) {
        ESP_LOGE(TAG, "opus_decoder_create failed: %d", err);
        s_dec = NULL;
    }

    s_tts_queue = xQueueCreate(48, sizeof(tts_item_t *));

    xTaskCreatePinnedToCore(stream_task, "opus_enc", STACK_OPUS_ENCODE, NULL, 6,
                            &s_stream_task, 1);
    xTaskCreatePinnedToCore(decode_task, "opus_dec", STACK_OPUS_DECODE, NULL, 5,
                            &s_decode_task, 1);

    ESP_LOGI(TAG, "Stream service initialized (enc=%p dec=%p)", s_enc, s_dec);
}

void stream_service_start(void) {
    if (s_active) return;

    if (s_enc) opus_encoder_ctl(s_enc, OPUS_RESET_STATE);
    if (s_dec) opus_decoder_ctl(s_dec, OPUS_RESET_STATE);

    s_enc_fill = 0;
    s_speech_seen = false;
    s_silence_ms = 0;
    s_stream_start_us = esp_timer_get_time();
    s_just_started = true;
    s_active = true;

    ESP_LOGI(TAG, "Streaming started");
}

void stream_service_stop(void) {
    if (!s_active) return;
    s_active = false;

    // Flush any partial frame (zero-padded) so nothing is lost.
    if (s_enc_fill > 0) {
        memset(s_enc_acc + s_enc_fill, 0,
               (OPUS_FRAME_SAMPLES - s_enc_fill) * sizeof(int16_t));
        send_opus_frame();
        s_enc_fill = 0;
    }

    ESP_LOGI(TAG, "Streaming stopped");
    xEventGroupSetBits(state_machine_get_events(), EVT_STREAM_END);
}

bool stream_service_is_active(void) {
    return s_active;
}

void stream_service_push_mic(const int16_t *samples, size_t count) {
    if (s_monitor == NULL) return;
    // Never block the audio path; drop if the monitor buffer is full.
    xRingbufferSend(s_monitor, samples, count * sizeof(int16_t), 0);
}

void stream_service_handle_tts(const uint8_t *data, size_t len) {
    if (len <= BINARY_HEADER_SIZE) return;

    binary_frame_header_t hdr;
    memcpy(&hdr, data, BINARY_HEADER_SIZE);
    if (hdr.version != BINARY_PROTO_VERSION || hdr.type != BINARY_TYPE_TTS_AUDIO) {
        return;
    }

    size_t payload_len = len - BINARY_HEADER_SIZE;
    if (payload_len == 0 || payload_len > 1500) return;

    tts_item_t *it = (tts_item_t *)malloc(sizeof(tts_item_t) + payload_len);
    if (it == NULL) return;
    it->len = payload_len;
    memcpy(it->data, data + BINARY_HEADER_SIZE, payload_len);

    if (xQueueSend(s_tts_queue, &it, 0) != pdTRUE) {
        free(it);  // queue full: drop frame
    }
}

void stream_service_flush_tts(void) {
    // Set the flag first so decode_task stops calling opus_decode() immediately.
    // Only after the task is guaranteed to be skipping (flag is checked before
    // every decode call) is it safe to touch the decoder state or drain the queue.
    s_flush_tts_req = true;

    audio_service_flush_playback();

    // Drain any queued TTS packets — decode_task is now skipping all of them.
    tts_item_t *it = NULL;
    while (xQueueReceive(s_tts_queue, &it, 0) == pdTRUE) {
        if (it) free(it);
    }

    // Safe to reset the decoder now: decode_task will not touch it until
    // s_flush_tts_req is cleared below.
    if (s_dec) opus_decoder_ctl(s_dec, OPUS_RESET_STATE);

    s_flush_tts_req = false;
}
