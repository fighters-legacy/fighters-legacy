// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

struct AtmosphereState {
    float density_kg_m3;
    float speed_of_sound_m_s;
    float pressure_pa;
};

// International Standard Atmosphere (ISO 2533) for 0–20 000 m — that path is BIT-IDENTICAL to the
// pre-#354 implementation, which fm-trim's CI gates and the generated manual depend on — extended
// with the US Standard Atmosphere 1976 layers to 86 000 m and vacuum above (#354, ballistic
// boost/coast/reentry). Negative altitudes clamp to sea level.
AtmosphereState computeAtmosphere(float altitude_m);

} // namespace fl
