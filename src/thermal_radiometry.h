#ifndef THERMAL_RADIOMETRY_H
#define THERMAL_RADIOMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Host/profile-side radiometry core.  This module deliberately has no Flash,
 * HTTP or frame-buffer dependency; integration into the TN160 runtime is a
 * later step behind a validated profile and explicit fallback. */
#define THERMAL_RADIOMETRY_MAX_NODES 8u
#define THERMAL_RADIOMETRY_NUC_T_LEGACY_COUNT 8192u
#define THERMAL_RADIOMETRY_NUC_T_ADVANCED_COUNT 16384u
#define THERMAL_RADIOMETRY_KT_Q14_ONE 16384u
#define THERMAL_RADIOMETRY_CALIBRATION_ANY 0u

enum thermal_radiometry_axis {
    THERMAL_RADIOMETRY_AXIS_NTC = 0,
    THERMAL_RADIOMETRY_AXIS_VTEMP = 1,
};

enum thermal_radiometry_state_flags {
    THERMAL_RADIOMETRY_STATE_CALIBRATION_VALID = 1u << 0,
    THERMAL_RADIOMETRY_STATE_GAIN_VALID = 1u << 1,
    THERMAL_RADIOMETRY_STATE_FFC_STABLE = 1u << 2,
};

#define THERMAL_RADIOMETRY_STATE_READY_MASK \
    (THERMAL_RADIOMETRY_STATE_CALIBRATION_VALID \
     | THERMAL_RADIOMETRY_STATE_GAIN_VALID \
     | THERMAL_RADIOMETRY_STATE_FFC_STABLE)

enum thermal_radiometry_status {
    THERMAL_RADIOMETRY_OK = 0,
    THERMAL_RADIOMETRY_INVALID_ARGUMENT,
    THERMAL_RADIOMETRY_INVALID_PROFILE,
    THERMAL_RADIOMETRY_STATE_NOT_READY,
    THERMAL_RADIOMETRY_GAIN_MISMATCH,
    THERMAL_RADIOMETRY_NUC_T_INVALID,
    THERMAL_RADIOMETRY_INPUT_OUT_OF_RANGE,
};

struct thermal_radiometry_node {
    uint16_t axis_adu;
    uint16_t kt_q14;
    int16_t bt_adu;
};

struct thermal_radiometry_profile {
    uint32_t calibration_id;
    uint16_t gain_mode;
    enum thermal_radiometry_axis axis;
    uint16_t node_count;
    const struct thermal_radiometry_node* nodes;
    uint16_t nuc_t_count;
    const uint16_t* nuc_t_kelvin_x16;
    uint32_t device_gain_q16;
    int32_t device_bias_adu;
};

struct thermal_radiometry_frame_state {
    uint16_t flags;
    uint16_t gain_mode;
    uint16_t ntc_adu;
    uint16_t vtemp_adu;
    uint32_t calibration_id;
};

struct thermal_radiometry_result {
    uint16_t input_nuc14;
    uint16_t corrected_nuc;
    uint16_t temperature_kelvin_x16;
    uint16_t kt_q14;
    int16_t bt_adu;
    bool state_clamped;
};

enum thermal_radiometry_status thermal_radiometry_validate_profile(
    const struct thermal_radiometry_profile* profile);

enum thermal_radiometry_status thermal_radiometry_apply(
    const struct thermal_radiometry_profile* profile,
    const struct thermal_radiometry_frame_state* state,
    uint16_t input_nuc14,
    struct thermal_radiometry_result* result);

const char* thermal_radiometry_status_string(
    enum thermal_radiometry_status status);

#endif
