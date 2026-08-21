#ifndef THERMAL_NCM_H
#define THERMAL_NCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define THERMAL_FRAME_WIDTH       160u
#define THERMAL_FRAME_HEIGHT      120u
#define THERMAL_FRAME_IMAGE_BYTES (THERMAL_FRAME_WIDTH * THERMAL_FRAME_HEIGHT)
#define THERMAL_FRAME_ADC16_BYTES (THERMAL_FRAME_IMAGE_BYTES * 2u)
#define SIPEED_THERMAL_META_BYTES 120u
#define SIPEED_THERMAL_TEMP8_WIRE_BYTES \
    (THERMAL_FRAME_IMAGE_BYTES + SIPEED_THERMAL_META_BYTES)
#define SIPEED_THERMAL_ADC16_WIRE_BYTES \
    (THERMAL_FRAME_ADC16_BYTES + SIPEED_THERMAL_META_BYTES)

enum sipeed_thermal_meta_flags {
    SIPEED_META_IMAGE_VALID = 1u << 0,
    SIPEED_META_CALIBRATED = 1u << 1,
    SIPEED_META_TEMP_TRANSFORM_VALID = 1u << 2,
    SIPEED_META_HLC_TEMPERATURES_VALID = 1u << 3,
    SIPEED_META_HLC_COORDINATES_VALID = 1u << 4,
    SIPEED_META_PAYLOAD_CRC_VALID = 1u << 5,
    SIPEED_META_DETECTOR_TEMPERATURE_VALID = 1u << 6,
    SIPEED_META_SOURCE_RANGE_VALID = 1u << 7,
    SIPEED_META_SOURCE_FRAME_VALID = 1u << 8,
    SIPEED_META_NUC_INDEX_VALID = 1u << 9,
    SIPEED_META_MEASUREMENT_CONFIG_VALID = 1u << 10,
    SIPEED_META_FFC_ACTIVE = 1u << 11,
    SIPEED_META_SHUTTER_CLOSED = 1u << 12,
    SIPEED_META_CALIBRATION_ID_VALID = 1u << 13,
};

enum sipeed_thermal_pixel_format {
    SIPEED_THERMAL_PIXEL_ADC16 = 1,
    SIPEED_THERMAL_PIXEL_RAW16 = 2,
    SIPEED_THERMAL_PIXEL_RAW8 = 3,
    SIPEED_THERMAL_PIXEL_TEMP16 = 4,
    SIPEED_THERMAL_PIXEL_TEMP8 = 5,
    SIPEED_THERMAL_PIXEL_JPGT = 6,
    SIPEED_THERMAL_PIXEL_JPG = 7,
};

/* Sipeed Thermal USB/NCM/HTTP protocol v1 metadata. All multi-byte fields
 * are IEEE-754/little-endian on the wire. meta_crc32 covers bytes 0..115. */
struct __attribute__((packed)) sipeed_thermal_meta_v1 {
    char magic[8];
    uint8_t version_major;
    uint8_t version_minor;
    uint16_t meta_bytes;
    uint32_t flags;
    uint32_t frame_number;
    uint32_t source_frame_number;
    uint32_t timestamp_ms;
    uint32_t payload_bytes;
    uint32_t payload_crc32;
    uint16_t width;
    uint16_t height;
    uint16_t stride_bytes;
    uint8_t pixel_format;
    uint8_t effective_bits;
    uint8_t display_rotation;
    uint8_t gain_mode;
    uint8_t range_method;
    uint8_t quant_max;
    float temperature_scale;
    float temperature_offset;
    float center_temperature;
    float min_temperature;
    float max_temperature;
    uint16_t center_x;
    uint16_t center_y;
    uint16_t min_x;
    uint16_t min_y;
    uint16_t max_x;
    uint16_t max_y;
    float detector_temperature;
    float source_low;
    float source_high;
    uint32_t calibration_id;
    uint16_t nuc_table_index;
    uint16_t ffc_count;
    uint8_t ffc_state;
    uint8_t shutter_state;
    uint16_t reserved0;
    uint32_t measurement_config_id;
    uint8_t reserved[8];
    uint32_t meta_crc32;
};

static_assert(sizeof(struct sipeed_thermal_meta_v1)
                  == SIPEED_THERMAL_META_BYTES,
              "unified thermal metadata must be exactly 120 bytes");
static_assert(offsetof(struct sipeed_thermal_meta_v1, flags) == 12);
static_assert(offsetof(struct sipeed_thermal_meta_v1, frame_number) == 16);
static_assert(offsetof(struct sipeed_thermal_meta_v1, source_frame_number)
                  == 20);
static_assert(offsetof(struct sipeed_thermal_meta_v1, timestamp_ms) == 24);
static_assert(offsetof(struct sipeed_thermal_meta_v1, payload_bytes) == 28);
static_assert(offsetof(struct sipeed_thermal_meta_v1, payload_crc32) == 32);
static_assert(offsetof(struct sipeed_thermal_meta_v1, width) == 36);
static_assert(offsetof(struct sipeed_thermal_meta_v1, pixel_format) == 42);
static_assert(offsetof(struct sipeed_thermal_meta_v1, quant_max) == 47);
static_assert(offsetof(struct sipeed_thermal_meta_v1, temperature_scale)
                  == 48);
static_assert(offsetof(struct sipeed_thermal_meta_v1, center_temperature)
                  == 56);
static_assert(offsetof(struct sipeed_thermal_meta_v1, center_x) == 68);
static_assert(offsetof(struct sipeed_thermal_meta_v1, detector_temperature)
                  == 80);
static_assert(offsetof(struct sipeed_thermal_meta_v1, source_low) == 84);
static_assert(offsetof(struct sipeed_thermal_meta_v1, calibration_id) == 92);
static_assert(offsetof(struct sipeed_thermal_meta_v1,
                       measurement_config_id) == 104);
static_assert(offsetof(struct sipeed_thermal_meta_v1, meta_crc32) == 116);

struct thermal_frame_view {
    const uint8_t* pixels;
    const struct sipeed_thermal_meta_v1* meta;
    uint8_t token;
};

typedef bool (*thermal_frame_acquire_fn)(struct thermal_frame_view* view);
typedef void (*thermal_frame_release_fn)(uint8_t token);
typedef void (*thermal_frame_cancel_fn)(void);

void thermal_ncm_init(thermal_frame_acquire_fn acquire,
                      thermal_frame_release_fn release,
                      thermal_frame_acquire_fn adc16_acquire,
                      thermal_frame_release_fn adc16_release,
                      thermal_frame_cancel_fn adc16_cancel);
void thermal_ncm_task(void);
void thermal_ncm_note_frame(uint8_t token, const uint8_t* normalized_frame,
                            bool calibrated);
void thermal_ncm_note_adc16(
    const uint8_t* adc16_pixels,
    const struct sipeed_thermal_meta_v1* source_meta);
const struct sipeed_thermal_meta_v1* thermal_ncm_meta(uint8_t token);
const struct sipeed_thermal_meta_v1* thermal_ncm_adc16_meta(void);

#endif
