#pragma once

#include "system/state_machine.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_manager_init(void);
void display_manager_set_state(device_state_t state);
void display_manager_show_transcript(const char *text, bool is_user);
void display_manager_show_status(const char *status);
void display_manager_set_brightness(int brightness);
void display_manager_set_theme(bool dark);

#ifdef __cplusplus
}
#endif
