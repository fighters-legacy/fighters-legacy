// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/IGravityField.h"

#include <array>
#include <cmath>

namespace fl {

// Spherical-Earth gravity field: 1/r² falloff toward a planet centre at {0, -R, 0}.
// GM is derived from the surface gravity and radius: GM = g_surface * R².
// All methods are inline so this header can be included in multiple translation units without a
// companion .cpp file.
class CentralGravityField final : public IGravityField {
    float m_GM;      // g_surface * R² (m³/s²)
    float m_centerY; // planet centre world-Y = -R

  public:
    explicit CentralGravityField(float R = 6'371'000.f, float g = 9.80665f) : m_GM(g * R * R), m_centerY(-R) {}

    std::array<float, 3> accelWorld(const float p[3]) const override {
        const float dx = p[0];
        const float dy = p[1] - m_centerY; // p[1] - (-R) = p[1] + R
        const float dz = p[2];
        const float r2 = dx * dx + dy * dy + dz * dz;
        if (r2 < 1.f)
            return {0.f, 0.f, 0.f}; // at or inside planet centre — avoid divide-by-zero
        const float r = std::sqrt(r2);
        const float a = -m_GM / r2; // magnitude (negative = toward centre)
        return {a * dx / r, a * dy / r, a * dz / r};
    }

    float geodeticAltitude(const float p[3]) const override {
        const float dx = p[0];
        const float dy = p[1] - m_centerY;
        const float dz = p[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz) + m_centerY; // |r_vec| - R
    }

    // Pre-built Earth instance (R = 6 371 000 m, g = 9.80665 m/s²).
    static const CentralGravityField& earthInstance() {
        static const CentralGravityField e{6'371'000.f, 9.80665f};
        return e;
    }
};

} // namespace fl
