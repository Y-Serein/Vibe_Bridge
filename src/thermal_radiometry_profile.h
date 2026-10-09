#ifndef THERMAL_RADIOMETRY_PROFILE_H
#define THERMAL_RADIOMETRY_PROFILE_H

#include <stdbool.h>

#include "thermal_radiometry.h"

/* A generated profile overrides this weak empty-provider implementation. */
bool thermal_radiometry_profile_get(
    struct thermal_radiometry_profile* profile);

#endif
