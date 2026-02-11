#include <cmap/cmap.h>
#include <stdint.h>

const uint8_t colormap_hot[] = {
#embed "./lut_colormap_hot.bin"
};

static_assert(sizeof(colormap_hot) == CMAP_SIZE, "CMAP size mismatch");
