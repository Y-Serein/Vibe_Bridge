#ifndef THERMAL_DRIFT_MODEL_H
#define THERMAL_DRIFT_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Keep the production/default path enabled. A diagnostic V1-only build may
 * set THERMAL_DRIFT_RUNTIME_ENABLE=0 through CMake; that disables both the
 * second-level NTC mean and the optional third-level B residual at runtime. */
#ifndef THERMAL_DRIFT_RUNTIME_ENABLE
#define THERMAL_DRIFT_RUNTIME_ENABLE 1
#endif

/* Three-layer thermal controller:
 *   1. V1 owns spatial background/gain and the base mean_diff.
 *   2. A batch-mean NTC anchor coefficient is available after V1 stores its
 *      per-device base NTC.
 *   3. An optional V1-bound record adds one per-device residual endpoint.
 *
 * The residual is linear from zero at base_ntc_adu to the measured values at
 * learned_ntc_adu. No independent PWL nodes are stored or accumulated. */
#define THERMAL_DRIFT_MODEL_BYTES                    64u
#define THERMAL_DRIFT_MODEL_VERSION                   2u
#define THERMAL_DRIFT_MODEL_FLAG_GAIN_RESIDUAL   0x0001u
#define THERMAL_DRIFT_MODEL_FLAG_CLEAR            0x0002u
#define THERMAL_DRIFT_MODEL_FLAGS_MASK            \
    (THERMAL_DRIFT_MODEL_FLAG_GAIN_RESIDUAL \
     | THERMAL_DRIFT_MODEL_FLAG_CLEAR)

/* Four-device batch mean from 2026-09-08 v1/v2/v4/v5, excluding the
 * user-identified outlier v3: 290 / 256 = 1.1328125 ADU per NTC code. */
#define THERMAL_DRIFT_MEAN_K_Q8                      290
#define THERMAL_DRIFT_MEAN_LIMIT_ADU                 600
#define THERMAL_DRIFT_MODEL_ANCHOR_LIMIT_ADU         800
#define THERMAL_DRIFT_MODEL_GAIN_Q16_LIMIT          9830
#define THERMAL_DRIFT_MODEL_GAIN_SCALE_MIN_Q16     55706
#define THERMAL_DRIFT_MODEL_GAIN_SCALE_MAX_Q16     75366
#define THERMAL_DRIFT_MODEL_MIN_NTC_SPAN_ADU          32u

struct __attribute__((packed)) thermal_drift_model_packet {
    char magic[8];               /* "T160DM2\\0" */
    uint16_t version;
    uint16_t bytes;
    uint32_t calibration_id;     /* exact active V1 transaction */
    uint16_t base_ntc_adu;       /* copied from active V1 metadata */
    uint16_t learned_ntc_adu;    /* state-B NTC */
    int16_t anchor_residual_adu; /* state-B residual after batch mean */
    int16_t gain_residual_q16;   /* state-B span residual vs V1 mean_diff */
    uint16_t flags;
    int16_t mean_k_q8;           /* binds record to compiled mean model */
    uint16_t mean_limit_adu;
    uint16_t total_anchor_limit_adu;
    uint8_t reserved[28];
    uint32_t crc32;
};

static_assert(sizeof(struct thermal_drift_model_packet)
                  == THERMAL_DRIFT_MODEL_BYTES,
              "thermal drift model packet must remain 64 bytes");

enum thermal_drift_model_result {
    THERMAL_DRIFT_MODEL_OK = 0,
    THERMAL_DRIFT_MODEL_BUSY,
    THERMAL_DRIFT_MODEL_BAD_SIZE,
    THERMAL_DRIFT_MODEL_BAD_PACKET,
    THERMAL_DRIFT_MODEL_FLASH_ERROR,
    THERMAL_DRIFT_MODEL_VERIFY_ERROR,
};

struct thermal_drift_model_manifest {
    bool valid;
    bool per_device;
    bool mean_fallback_available;
    uint32_t generation;
    uint32_t calibration_id;
    uint32_t packet_crc32;
    uint16_t base_ntc_adu;
    uint16_t learned_ntc_adu;
    int16_t mean_k_q8;
};

struct thermal_drift_model_runtime {
    bool active;
    bool mean_active;
    bool per_device;
    bool ntc_clamped;
    int16_t mean_anchor_comp_adu;
    int16_t device_anchor_residual_adu;
    int16_t anchor_comp_adu;
    int16_t gain_delta_q16;
    int32_t gain_scale_q16;
};

extern volatile uint8_t g_thermal_drift_model_valid;
extern volatile uint8_t g_thermal_drift_model_active;
extern volatile uint8_t g_thermal_drift_model_mean_active;
extern volatile uint8_t g_thermal_drift_model_per_device;
extern volatile uint8_t g_thermal_drift_model_ntc_clamped;
extern volatile uint16_t g_thermal_drift_model_base_ntc_adu;
extern volatile uint16_t g_thermal_drift_model_ntc_min_adu;
extern volatile uint16_t g_thermal_drift_model_ntc_max_adu;
extern volatile int16_t g_thermal_drift_model_mean_k_q8;
extern volatile int16_t g_thermal_drift_model_mean_anchor_comp_adu;
extern volatile int16_t g_thermal_drift_model_device_anchor_residual_adu;
extern volatile int16_t g_thermal_drift_model_anchor_comp_adu;
extern volatile int16_t g_thermal_drift_model_gain_delta_q16;
extern volatile int32_t g_thermal_drift_model_gain_scale_q16;

void thermal_drift_model_boot_load(void);
struct thermal_drift_model_runtime thermal_drift_model_evaluate(
    uint16_t current_ntc_adu);
bool thermal_drift_model_packet(
    uint8_t out[THERMAL_DRIFT_MODEL_BYTES]);
bool thermal_drift_model_manifest(struct thermal_drift_model_manifest* out);
enum thermal_drift_model_result thermal_drift_model_write(
    const uint8_t* data, size_t bytes);
/* Commit a journaled tombstone for the active V1 calibration.  The previous
 * model remains the boot/runtime fallback until this record verifies. */
enum thermal_drift_model_result thermal_drift_model_clear(void);
const char* thermal_drift_model_result_string(
    enum thermal_drift_model_result result);

#endif
