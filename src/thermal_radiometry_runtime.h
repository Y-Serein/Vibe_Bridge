#ifndef THERMAL_RADIOMETRY_RUNTIME_H
#define THERMAL_RADIOMETRY_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "thermal_radiometry.h"

extern volatile uint8_t g_thermal_radiometry_available;
extern volatile uint8_t g_thermal_radiometry_active;
extern volatile uint8_t g_thermal_radiometry_clamped;
extern volatile uint16_t g_thermal_radiometry_axis_adu;
extern volatile uint16_t g_thermal_radiometry_index;
extern volatile uint16_t g_thermal_radiometry_kt_q14;
extern volatile int16_t g_thermal_radiometry_bt_adu;
extern volatile uint16_t g_thermal_radiometry_corrected_nuc;
extern volatile uint16_t g_thermal_radiometry_temperature_kelvin_x16;

void thermal_radiometry_runtime_boot_load(void);
bool thermal_radiometry_runtime_apply(
    uint16_t input_nuc14,
    uint16_t ntc_adu,
    uint16_t vtemp_adu,
    uint16_t gain_mode,
    uint32_t calibration_id,
    bool ffc_stable,
    struct thermal_radiometry_result* result);

#endif
