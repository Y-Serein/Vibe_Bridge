#pragma once

#include <stdint.h>

void led_init();
void led_deinit();
void led_set(const bool state);
uint8_t led_get();
void led_on();
void led_off();
void led_toggle();
void led_on_event_blocking(const uint8_t cnt, const uint32_t inv);
void led_on_failed(const uint8_t cnt);
void led_on_succ(const uint8_t cnt);
