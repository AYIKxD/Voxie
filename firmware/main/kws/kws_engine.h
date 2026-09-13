#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Keyword Spotting Engine
 * Allocates TFLite resources and initializes model buffers.
 */
void kws_engine_init(void);

/**
 * @brief Start the KWS engine tasks
 * Spawns the feature extraction and inference tasks on Core 0.
 */
void kws_engine_start(void);

/**
 * @brief Stop the KWS engine tasks
 */
void kws_engine_stop(void);

#ifdef __cplusplus
}
#endif
