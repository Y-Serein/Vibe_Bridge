#ifndef THERMAL_WEB_ASSETS_H
#define THERMAL_WEB_ASSETS_H

#include <stddef.h>
#include <stdint.h>

extern const uint8_t thermal_web_index_start[];
extern const uint8_t thermal_web_index_end[];
extern const uint8_t thermal_sr_fast_wasm_start[];
extern const uint8_t thermal_sr_fast_wasm_end[];
extern const uint8_t thermal_sr_fast_simd_wasm_start[];
extern const uint8_t thermal_sr_fast_simd_wasm_end[];
extern const uint8_t thermal_sr_fast_weights_start[];
extern const uint8_t thermal_sr_fast_weights_end[];

static inline size_t thermal_web_asset_size(const uint8_t* start,
                                            const uint8_t* end)
{
    return (size_t)(end - start);
}

#endif
