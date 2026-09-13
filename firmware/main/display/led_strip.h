#pragma once

#include "system/state_machine.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void led_strip_init(void);
void led_strip_set_state(device_state_t state);
void led_strip_set_color(uint8_t r, uint8_t g, uint8_t b);
void led_strip_set_effect(const char *effect);

#ifdef __cplusplus
}
#endif
