#pragma once

#include <stdint.h>

#define CMAP_W      256
#define CMAP_C      3
#define CMAP_SIZE   (CMAP_W * CMAP_C)
#define CMAP_BYTES  CMAP_SIZE
#define CMAP_SMOOTH 1

#define CMAP_NUM 6
extern const uint8_t colormap_hot[];
extern const uint8_t colormap_cool[];
extern const uint8_t colormap_magma[];
extern const uint8_t colormap_turbo[];
extern const uint8_t colormap_deepgreen[];
extern const uint8_t colormap_gray[];
