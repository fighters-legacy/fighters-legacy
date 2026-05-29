// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace fl {

enum class DamageLevel : uint8_t { Intact = 0, Light, Heavy, Critical, Destroyed };

// Per-threshold configuration loaded from TOML. Visual effects and penalty multipliers
// apply while an entity remains at or below hpFraction.
struct DamagePenalty {
    float hpFraction{1.f};    // HP fraction at which this level begins (0,1]
    std::string visualEffect; // particle effect asset name; empty = none
    float thrustFactor{1.f};  // multiplier on engine thrust [0,1]
    float controlFactor{1.f}; // multiplier on control surface authority [0,1]
    bool avionicsFailure{false};
};

struct DamageDef {
    DamagePenalty light;
    DamagePenalty heavy;
    DamagePenalty critical;
};

// Maps a current HP fraction to the appropriate DamageLevel.
// Returns Destroyed when hpFraction <= 0, Intact when above the light threshold.
inline DamageLevel evaluateDamageLevel(const DamageDef& def, float hpFraction) noexcept {
    if (hpFraction <= 0.f)
        return DamageLevel::Destroyed;
    if (hpFraction <= def.critical.hpFraction)
        return DamageLevel::Critical;
    if (hpFraction <= def.heavy.hpFraction)
        return DamageLevel::Heavy;
    if (hpFraction <= def.light.hpFraction)
        return DamageLevel::Light;
    return DamageLevel::Intact;
}

} // namespace fl
