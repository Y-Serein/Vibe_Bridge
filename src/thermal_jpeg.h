#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum thermal_jpeg_mode {
    THERMAL_JPEG_GRAY = 1,
    THERMAL_JPEG_TURBO = 2,
};

#define THERMAL_JPEG_QUALITY 80u

struct thermal_jpeg_result {
    size_t bytes;
    bool overflow;
};

/* Encode normalized 8-bit thermal pixels as baseline JPEG. Turbo mode applies
 * the firmware RGB LUT while loading each 8x8 MCU, so it needs no RGB frame
 * buffer. Color uses 4:2:0 chroma subsampling; grayscale is single-component. */
bool thermal_jpeg_encode(const uint8_t* gray, uint16_t width, uint16_t height,
                         enum thermal_jpeg_mode mode, uint8_t quality,
                         uint8_t* output, size_t output_capacity,
                         struct thermal_jpeg_result* result);
