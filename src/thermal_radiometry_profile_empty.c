#include "thermal_radiometry_profile.h"

__attribute__((weak)) bool thermal_radiometry_profile_get(
    struct thermal_radiometry_profile* profile)
{
    (void)profile;
    return false;
}
