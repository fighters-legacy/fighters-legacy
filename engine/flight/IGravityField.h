// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

namespace fl {

// World-frame gravitational acceleration field. The default FlatGravityField is uniform (flat-Earth,
// 9.80665 m/s^2 down). A central/spherical field (planet-relative, 1/r^2 falloff toward the planet
// centre) plugs in here for MRBM/ICBM-scale ballistic trajectories — the spherical-Earth world model
// (issue #357) — without touching FlightIntegrator. Kept on float[3] to match the integrator's
// GLM-free state vector.
struct IGravityField {
    virtual ~IGravityField() = default;
    // Gravitational acceleration (m/s^2) in the world frame [x=forward, y=up, z=right] at a world
    // position. Uniform fields ignore the position.
    virtual std::array<float, 3> accelWorld(const float pos_world[3]) const = 0;
    // Geodetic (pressure) altitude (m) at a world position.
    // Default: world-Y (correct for a flat planet with the sea-level plane at Y=0).
    // Spherical fields override this to return the true MSL altitude far from the origin.
    virtual float geodeticAltitude(const float pos_world[3]) const {
        return pos_world[1];
    }
};

// Uniform flat-Earth gravity: constant 9.80665 m/s^2 along -world_y. The default for every integrator.
class FlatGravityField final : public IGravityField {
  public:
    std::array<float, 3> accelWorld(const float /*pos_world*/[3]) const override {
        return {0.f, -9.80665f, 0.f};
    }
    static const FlatGravityField& instance() {
        static const FlatGravityField f;
        return f;
    }
};

} // namespace fl
