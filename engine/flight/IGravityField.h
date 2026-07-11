// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

namespace fl {

// World-frame gravitational acceleration field. CentralGravityField (1/r² falloff toward planet
// centre at {0,-R,0}) is the production implementation. Plug in a custom field via
// FlightIntegrator::setGravityField() for exotic planets.
struct IGravityField {
    virtual ~IGravityField() = default;
    // Gravitational acceleration (m/s^2) in the world frame [x=forward, y=up, z=right] at a world
    // position. Return stays float — acceleration magnitudes are small and float precision suffices.
    virtual std::array<float, 3> accelWorld(const double pos_world[3]) const = 0;
    // Geodetic (pressure) altitude (m) at a world position. Returns double for accuracy at
    // planet-radius scale. Default: world-Y (flat planet, sea-level plane at Y=0).
    // CentralGravityField overrides this to return the true MSL altitude.
    virtual double geodeticAltitude(const double pos_world[3]) const {
        return pos_world[1];
    }
    // Local "up": unit vector (world frame) pointing away from the planet surface at a world
    // position — the radial direction on a spherical planet. Used by the ground-collision floor
    // to test/snap along the true vertical anywhere on the planet (#477). Default: world +Y
    // (flat planet). CentralGravityField overrides this with the radial direction.
    virtual std::array<float, 3> geodeticUp(const double /*pos_world*/[3]) const {
        return {0.f, 1.f, 0.f};
    }
};

} // namespace fl
