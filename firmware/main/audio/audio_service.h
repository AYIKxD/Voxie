#pragma once

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/**
 * Audio Service — manages I2S mic capture (RX) and speaker playback (TX).
 *
 * Creates two FreeRTOS tasks:
 *   - i2s_mic_task:   reads I2S DMA → pushes PCM into mic ring buffer
 *   - playback_task:  pulls PCM from playback ring buffer → writes I2S DMA
 *
 * The mic ring buffer is consumed by:
 *   - KWS feature extractor (always)
 *   - Opus encoder → WebSocket (only when streaming)
 *
 * The playback ring buffer is fed by:
 *   - Opus decoder (TTS audio from cloud)
 */

/// Initialize I2S peripherals, DMA buffers, and ring buffers. Call once from app_main.
void audio_service_init(void);

/// Start the mic capture and playback tasks
void audio_service_start(void);

/// Read PCM samples from the mic ring buffer (blocking, up to timeout_ms).
/// Returns number of bytes actually read.
size_t audio_service_read_mic(int16_t *buf, size_t num_samples, uint32_t timeout_ms);

/// Write PCM samples to the playback ring buffer for speaker output.
/// Returns true if all samples were written within timeout.
bool audio_service_write_playback(const int16_t *buf, size_t num_samples, uint32_t timeout_ms);

/// Flush the playback buffer (used on barge-in / abort)
void audio_service_flush_playback(void);

/// Get the mic ring buffer handle (for direct access by KWS pre-roll)
RingbufHandle_t audio_service_get_mic_ringbuf(void);

/// Set speaker volume (0–100). Applies software gain scaling.
void audio_service_set_volume(int volume);

/// Get current volume (0–100)
int audio_service_get_volume(void);

/// True while the speaker is actively producing audio (plus a short tail).
/// Used to suppress the wake-word detector so the device does not trigger on
/// its own TTS output (there is no acoustic echo cancellation).
bool audio_service_is_playing(void);
