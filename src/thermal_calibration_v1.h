#ifndef THERMAL_CALIBRATION_V1_H
#define THERMAL_CALIBRATION_V1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Host-visible package format used by the production calibration tool.
 * The package is deliberately separate from the on-Flash FlashDataInfo V1
 * layout so the latter remains compatible with the existing thermal path. */
#define THERMAL_CALIBRATION_V1_HEADER_BYTES     64u
#define THERMAL_CALIBRATION_V1_IMAGE_BYTES      (160u * 120u * 2u)
#define THERMAL_CALIBRATION_V1_GAIN_BYTES       (160u * 120u * 4u)
#define THERMAL_CALIBRATION_V1_BADPOINT_BYTES   10u
#define THERMAL_CALIBRATION_V1_MEAN_DIFF_MIN     100.0f
#define THERMAL_CALIBRATION_V1_PACKET_BYTES                         \
    (THERMAL_CALIBRATION_V1_HEADER_BYTES                              \
     + THERMAL_CALIBRATION_V1_IMAGE_BYTES                             \
     + THERMAL_CALIBRATION_V1_GAIN_BYTES                              \
     + THERMAL_CALIBRATION_V1_BADPOINT_BYTES)

/* Optional tagged eight-byte extension inside TNC1 header bytes 52..59.
 * All-zero bytes retain the base V1 meaning.  Legacy "TN" remains readable
 * but inactive; "TC" carries the signed per-device cold-start transient. */
#define THERMAL_CALIBRATION_V1_NTC_MODEL_TAG       0x4e54u /* "TN" */
#define THERMAL_CALIBRATION_V1_NTC_MODEL_K_Q8_MAX  8192
#define THERMAL_CALIBRATION_V1_NTC_MODEL_LIMIT_MAX 1500u
#define THERMAL_CALIBRATION_V1_NTC_MODEL_OFFSET    52u
#define THERMAL_CALIBRATION_V1_STARTUP_MODEL_TAG   0x4354u /* "TC" */
#define THERMAL_CALIBRATION_V1_STARTUP_AMPLITUDE_MAX 600
#define THERMAL_CALIBRATION_V1_STARTUP_TAU_DS_MIN  50u
#define THERMAL_CALIBRATION_V1_STARTUP_TAU_DS_MAX  600u

struct __attribute__((packed)) thermal_calibration_v1_ntc_model {
    uint16_t tag;
    uint16_t reference_adu;
    int16_t k_q8;
    uint16_t limit_adu;
};

static_assert(sizeof(struct thermal_calibration_v1_ntc_model) == 8,
              "NTC model extension must fit TNC1 reserved bytes");

/* Same eight bytes under the "TC" tag.  amplitude_adu is signed and tau_ds
 * uses 0.1-second units. */
struct __attribute__((packed)) thermal_calibration_v1_startup_model {
    uint16_t tag;
    uint16_t cold_ntc_threshold_adu;
    int16_t amplitude_adu;
    uint16_t tau_ds;
};

static_assert(sizeof(struct thermal_calibration_v1_startup_model) == 8,
              "startup model extension must fit TNC1 reserved bytes");

enum thermal_calibration_v1_result {
    THERMAL_CALIBRATION_V1_OK = 0,
    THERMAL_CALIBRATION_V1_BUSY,
    THERMAL_CALIBRATION_V1_BAD_SIZE,
    THERMAL_CALIBRATION_V1_BAD_HEADER,
    THERMAL_CALIBRATION_V1_BAD_DATA,
    THERMAL_CALIBRATION_V1_FLASH_ERROR,
    THERMAL_CALIBRATION_V1_VERIFY_ERROR,
};

enum thermal_calibration_v1_data_kind {
    THERMAL_CALIBRATION_V1_DATA_IMAGE = 0,
    THERMAL_CALIBRATION_V1_DATA_GAIN,
    THERMAL_CALIBRATION_V1_DATA_BADPOINTS,
};

struct thermal_calibration_v1_manifest {
    bool valid;
    uint32_t generation;
    uint32_t calibration_id;
    uint32_t image_crc32;
    uint32_t gain_crc32;
    uint32_t badpoint_crc32;
    uint32_t payload_crc32;
    uint32_t package_header_crc32;
    uint32_t flash_header_crc32;
};

struct thermal_calibration_v1_commit {
    uint32_t generation;
    uint32_t calibration_id;
    uint32_t payload_crc32;
    uint32_t package_header_crc32;
    uint32_t flash_header_crc32;
};

typedef bool (*thermal_calibration_v1_precommit_fn)(void* context);

/* Call once before the application reads flash_header/flash_imgbg/etc. */
void thermal_calibration_v1_boot_select(void);

/* True only when boot selection accepted either a complete selector-backed
 * transaction or the known legacy primary layout.  Callers must not use a
 * merely plausible FlashDataInfo after a rejected partial selector write. */
bool thermal_calibration_v1_boot_has_calibration(void);

/* Streaming upload interface. Exactly one packet is accepted per transaction.
 * The caller must abort after a disconnected or failed HTTP request. */
enum thermal_calibration_v1_result thermal_calibration_v1_begin(
    uint32_t packet_bytes);
enum thermal_calibration_v1_result thermal_calibration_v1_write(
    const uint8_t* data, size_t bytes);
enum thermal_calibration_v1_result thermal_calibration_v1_finish(
    struct thermal_calibration_v1_commit* commit);
void thermal_calibration_v1_abort(void);
enum thermal_calibration_v1_result
thermal_calibration_v1_finish_with_precommit(
    struct thermal_calibration_v1_commit* commit,
    thermal_calibration_v1_precommit_fn precommit, void* context);

bool thermal_calibration_v1_active(void);
bool thermal_calibration_v1_upload_active(void);
const char* thermal_calibration_v1_result_string(
    enum thermal_calibration_v1_result result);

/* Readback helpers for the NCM HTTP endpoint. `header` receives a freshly
 * reconstructed 64-byte TNC1 package header. */
bool thermal_calibration_v1_manifest(
    struct thermal_calibration_v1_manifest* manifest);
bool thermal_calibration_v1_make_packet_header(
    uint8_t header[THERMAL_CALIBRATION_V1_HEADER_BYTES]);
bool thermal_calibration_v1_data(enum thermal_calibration_v1_data_kind kind,
                                 const uint8_t** data, uint32_t* bytes);
uint32_t thermal_calibration_v1_id(void);
bool thermal_calibration_v1_ntc_model(
    struct thermal_calibration_v1_ntc_model* model);
bool thermal_calibration_v1_startup_model(
    struct thermal_calibration_v1_startup_model* model);

#endif
