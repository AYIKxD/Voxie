#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void afe_service_init(void);   // call once at startup, before audio tasks
void afe_service_start(void);  // start the AFE processing task
void afe_service_stop(void);   // stop gracefully
bool afe_service_is_wake_word_detected(void); // polled by main event loop

#ifdef __cplusplus
}
#endif
