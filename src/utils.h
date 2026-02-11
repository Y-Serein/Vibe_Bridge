#pragma once

#include <stdint.h>

int32_t make_line(const uint16_t* line, uint8_t** out, uint32_t* outs);
int32_t make_line0(const uint16_t* line, uint8_t** out, uint32_t* outs);
void calil(const uint8_t* in, uint8_t** out, uint32_t* outsiz);
