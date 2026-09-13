#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize Opus encoder/decoder and the streaming tasks. Call once at boot.
void stream_service_init(void);

/// Begin streaming the pre-roll + live mic audio to the cloud (on wake word).
void stream_service_start(void);

/// Stop streaming and signal end-of-stream to the application.
void stream_service_stop(void);

/// True while mic audio is being streamed upstream.
bool stream_service_is_active(void);

/// Feed raw mic samples into the streaming/pre-roll pipeline (called by the
/// feature task; never blocks).
void stream_service_push_mic(const int16_t *samples, size_t count);

/// Handle an incoming binary frame from the server (Opus TTS audio).
void stream_service_handle_tts(const uint8_t *data, size_t len);

/// Flush queued/playing TTS audio (barge-in).
void stream_service_flush_tts(void);

#ifdef __cplusplus
}
#endif
