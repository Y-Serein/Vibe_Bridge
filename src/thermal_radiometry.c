#include "thermal_radiometry.h"

#include <string.h>

static int32_t round_div_i64(int64_t numerator, int32_t denominator)
{
    if (numerator >= 0) {
        return (int32_t)((numerator + denominator / 2) / denominator);
    }
    return -(int32_t)(((-numerator) + denominator / 2) / denominator);
}

static bool nuc_t_count_valid(uint16_t count)
{
    return count == THERMAL_RADIOMETRY_NUC_T_LEGACY_COUNT
           || count == THERMAL_RADIOMETRY_NUC_T_ADVANCED_COUNT;
}

static bool axis_kind_valid(enum thermal_radiometry_axis axis)
{
    return axis == THERMAL_RADIOMETRY_AXIS_NTC
           || axis == THERMAL_RADIOMETRY_AXIS_VTEMP;
}

enum thermal_radiometry_status thermal_radiometry_validate_profile(
    const struct thermal_radiometry_profile* profile)
{
    if (!profile || !profile->nodes || !profile->nuc_t_kelvin_x16
        || !axis_kind_valid(profile->axis)
        || profile->node_count < 2u
        || profile->node_count > THERMAL_RADIOMETRY_MAX_NODES
        || !nuc_t_count_valid(profile->nuc_t_count)) {
        return THERMAL_RADIOMETRY_INVALID_PROFILE;
    }
    if (profile->gain_mode != 0u && profile->gain_mode != 1u
        && profile->gain_mode != 0xffu) {
        return THERMAL_RADIOMETRY_INVALID_PROFILE;
    }
    if (profile->device_gain_q16 == 0u) {
        return THERMAL_RADIOMETRY_INVALID_PROFILE;
    }
    for (uint16_t i = 0; i < profile->node_count; ++i) {
        const struct thermal_radiometry_node* node = &profile->nodes[i];
        if (node->kt_q14 == 0u) {
            return THERMAL_RADIOMETRY_INVALID_PROFILE;
        }
        if (i > 0u && node->axis_adu <= profile->nodes[i - 1u].axis_adu) {
            return THERMAL_RADIOMETRY_INVALID_PROFILE;
        }
    }
    for (uint16_t i = 1; i < profile->nuc_t_count; ++i) {
        if (profile->nuc_t_kelvin_x16[i]
                < profile->nuc_t_kelvin_x16[i - 1u]) {
            return THERMAL_RADIOMETRY_NUC_T_INVALID;
        }
    }
    return THERMAL_RADIOMETRY_OK;
}

static uint16_t state_axis_value(
    const struct thermal_radiometry_profile* profile,
    const struct thermal_radiometry_frame_state* state)
{
    return profile->axis == THERMAL_RADIOMETRY_AXIS_VTEMP
        ? state->vtemp_adu : state->ntc_adu;
}

static void interpolate_node(
    const struct thermal_radiometry_profile* profile,
    uint16_t axis,
    uint16_t* kt_q14,
    int16_t* bt_adu,
    bool* clamped)
{
    const struct thermal_radiometry_node* nodes = profile->nodes;
    uint16_t left = 0u;
    uint16_t right = 0u;
    if (axis <= nodes[0].axis_adu) {
        right = 0u;
        if (axis < nodes[0].axis_adu && clamped) *clamped = true;
    } else if (axis >= nodes[profile->node_count - 1u].axis_adu) {
        left = profile->node_count - 1u;
        if (axis > nodes[left].axis_adu && clamped) *clamped = true;
    } else {
        for (uint16_t i = 1u; i < profile->node_count; ++i) {
            if (axis <= nodes[i].axis_adu) {
                left = (uint16_t)(i - 1u);
                right = i;
                break;
            }
        }
    }
    if (left == right) {
        *kt_q14 = nodes[left].kt_q14;
        *bt_adu = nodes[left].bt_adu;
        return;
    }
    const int32_t span = (int32_t)nodes[right].axis_adu
                         - (int32_t)nodes[left].axis_adu;
    const int32_t position = (int32_t)axis
                             - (int32_t)nodes[left].axis_adu;
    *kt_q14 = (uint16_t)round_div_i64(
        (int64_t)nodes[left].kt_q14 * span
            + (int64_t)((int32_t)nodes[right].kt_q14
                        - (int32_t)nodes[left].kt_q14)
                  * position,
        span);
    *bt_adu = (int16_t)round_div_i64(
        (int64_t)nodes[left].bt_adu * span
            + (int64_t)((int32_t)nodes[right].bt_adu
                        - (int32_t)nodes[left].bt_adu)
                  * position,
        span);
}

enum thermal_radiometry_status thermal_radiometry_apply(
    const struct thermal_radiometry_profile* profile,
    const struct thermal_radiometry_frame_state* state,
    uint16_t input_nuc14,
    struct thermal_radiometry_result* result)
{
    if (!state || !result) return THERMAL_RADIOMETRY_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));
    enum thermal_radiometry_status status =
        thermal_radiometry_validate_profile(profile);
    if (status != THERMAL_RADIOMETRY_OK) return status;
    if ((state->flags & THERMAL_RADIOMETRY_STATE_READY_MASK)
            != THERMAL_RADIOMETRY_STATE_READY_MASK
        || (profile->calibration_id != THERMAL_RADIOMETRY_CALIBRATION_ANY
            && state->calibration_id != profile->calibration_id)) {
        return THERMAL_RADIOMETRY_STATE_NOT_READY;
    }
    if (profile->gain_mode != 0xffu && state->gain_mode != profile->gain_mode) {
        return THERMAL_RADIOMETRY_GAIN_MISMATCH;
    }
    if (input_nuc14 > 16383u) {
        return THERMAL_RADIOMETRY_INPUT_OUT_OF_RANGE;
    }
    bool clamped = false;
    uint16_t kt_q14 = 0u;
    int16_t bt_adu = 0;
    interpolate_node(profile, state_axis_value(profile, state), &kt_q14,
                     &bt_adu, &clamped);
    int32_t corrected = (int32_t)(((int64_t)kt_q14 * input_nuc14 + 8192)
                                  >> 14) + bt_adu;
    corrected = (int32_t)(((int64_t)corrected * profile->device_gain_q16
                           + 32768) >> 16)
                + profile->device_bias_adu;
    if (corrected < 0) corrected = 0;
    if (corrected >= profile->nuc_t_count) {
        corrected = profile->nuc_t_count - 1u;
        clamped = true;
    }
    result->input_nuc14 = input_nuc14;
    result->corrected_nuc = (uint16_t)corrected;
    result->temperature_kelvin_x16 = profile->nuc_t_kelvin_x16[corrected];
    result->kt_q14 = kt_q14;
    result->bt_adu = bt_adu;
    result->state_clamped = clamped;
    return THERMAL_RADIOMETRY_OK;
}

const char* thermal_radiometry_status_string(
    enum thermal_radiometry_status status)
{
    static const char* const names[] = {
        "ok", "invalid-argument", "invalid-profile", "state-not-ready",
        "gain-mismatch", "nuc-t-invalid", "input-out-of-range",
    };
    return (unsigned)status < sizeof(names) / sizeof(names[0])
        ? names[status] : "unknown";
}
