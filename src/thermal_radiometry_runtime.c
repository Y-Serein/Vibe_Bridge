#include "thermal_radiometry_runtime.h"

#include <string.h>

#include "thermal_radiometry_profile.h"

volatile uint8_t g_thermal_radiometry_available = 0;
volatile uint8_t g_thermal_radiometry_active = 0;
volatile uint8_t g_thermal_radiometry_clamped = 0;
volatile uint16_t g_thermal_radiometry_axis_adu = 0;
volatile uint16_t g_thermal_radiometry_index = UINT16_MAX;
volatile uint16_t g_thermal_radiometry_kt_q14 = 0;
volatile int16_t g_thermal_radiometry_bt_adu = 0;
volatile uint16_t g_thermal_radiometry_corrected_nuc = 0;
volatile uint16_t g_thermal_radiometry_temperature_kelvin_x16 = 0;

static struct thermal_radiometry_profile runtime_profile;

void thermal_radiometry_runtime_boot_load(void)
{
    memset(&runtime_profile, 0, sizeof(runtime_profile));
    g_thermal_radiometry_available =
        thermal_radiometry_profile_get(&runtime_profile)
        && thermal_radiometry_validate_profile(&runtime_profile)
               == THERMAL_RADIOMETRY_OK;
    g_thermal_radiometry_active = 0;
    g_thermal_radiometry_clamped = 0;
    g_thermal_radiometry_axis_adu = 0;
    g_thermal_radiometry_index = UINT16_MAX;
    g_thermal_radiometry_kt_q14 = 0;
    g_thermal_radiometry_bt_adu = 0;
    g_thermal_radiometry_corrected_nuc = 0;
    g_thermal_radiometry_temperature_kelvin_x16 = 0;
}

bool thermal_radiometry_runtime_apply(
    uint16_t input_nuc14,
    uint16_t ntc_adu,
    uint16_t vtemp_adu,
    uint16_t gain_mode,
    uint32_t calibration_id,
    bool ffc_stable,
    struct thermal_radiometry_result* result)
{
    if (!result || !g_thermal_radiometry_available) {
        g_thermal_radiometry_active = 0;
        return false;
    }
    const struct thermal_radiometry_frame_state state = {
        .flags = (uint16_t)(THERMAL_RADIOMETRY_STATE_CALIBRATION_VALID
                            | THERMAL_RADIOMETRY_STATE_GAIN_VALID
                            | (ffc_stable
                                   ? THERMAL_RADIOMETRY_STATE_FFC_STABLE
                                   : 0u)),
        .gain_mode = gain_mode,
        .ntc_adu = ntc_adu,
        .vtemp_adu = vtemp_adu,
        .calibration_id = calibration_id,
    };
    const enum thermal_radiometry_status status =
        thermal_radiometry_apply(&runtime_profile, &state, input_nuc14,
                                 result);
    if (status != THERMAL_RADIOMETRY_OK) {
        g_thermal_radiometry_active = 0;
        return false;
    }
    g_thermal_radiometry_active = 1;
    g_thermal_radiometry_clamped = result->state_clamped ? 1u : 0u;
    g_thermal_radiometry_axis_adu = runtime_profile.axis
        == THERMAL_RADIOMETRY_AXIS_VTEMP ? vtemp_adu : ntc_adu;
    g_thermal_radiometry_index = result->corrected_nuc;
    g_thermal_radiometry_kt_q14 = result->kt_q14;
    g_thermal_radiometry_bt_adu = result->bt_adu;
    g_thermal_radiometry_corrected_nuc = result->corrected_nuc;
    g_thermal_radiometry_temperature_kelvin_x16 =
        result->temperature_kelvin_x16;
    return true;
}
