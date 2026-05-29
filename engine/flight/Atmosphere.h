// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

struct AtmosphereState {
    float density_kg_m3;
    float speed_of_sound_m_s;
    float pressure_pa;
};

// International Standard Atmosphere (ISO 2533).
// Valid for 0–20 000 m. Values are clamped at the boundaries.
AtmosphereState computeAtmosphere(float altitude_m);

} // namespace fl
