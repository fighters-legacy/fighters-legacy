// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

namespace fl {

// World-frame gravitational acceleration field. CentralGravityField (1/r² falloff toward planet
// centre at {0,-R,0}) is the production implementation. Plug in a custom field via
// FlightIntegrator::setGravityField() for exotic planets. Kept on float[3] to match the integrator's
// GLM-free state vector.
struct IGravityField {
    virtual ~IGravityField() = default;
    // Gravitational acceleration (m/s^2) in the world frame [x=forward, y=up, z=right] at a world
    // position.
    virtual std::array<float, 3> accelWorld(const float pos_world[3]) const = 0;
    // Geodetic (pressure) altitude (m) at a world position.
    // Default: world-Y (correct for a flat planet with the sea-level plane at Y=0).
    // CentralGravityField overrides this to return the true MSL altitude.
    virtual float geodeticAltitude(const float pos_world[3]) const {
        return pos_world[1];
    }
};

} // namespace fl
