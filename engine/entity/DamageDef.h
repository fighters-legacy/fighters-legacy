// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace fl {

enum class DamageLevel : uint8_t { Intact = 0, Light, Heavy, Critical, Destroyed };

// The fixed subsystem vocabulary (#675). A closed set, not free-form: every effect maps onto
// machinery the sim already has (asymmetric-thrust engine flags, control authority, avionics
// failure, fuel leak), so a pack cannot invent a subsystem the engine would not know how to fail.
// EngineLeft/EngineRight are the TWIN-engine asymmetric case (a kill halves thrust and yaws toward
// the dead side); Engine (#901) is the CENTRELINE single-engine case (a kill is total thrust loss and
// no yaw — there is no dead side to swing toward). Single-engine content declares [damage.subsystems]
// .engine; twin content declares .engine_left / .engine_right.
enum class Subsystem : uint8_t { EngineLeft = 0, EngineRight, Controls, Avionics, Hydraulics, Fuel, Engine, Count };
inline constexpr int kSubsystemCount = static_cast<int>(Subsystem::Count);

// One subsystem's authored parameters. hp = 0 means "this entity does not model this subsystem"
// (it is skipped entirely — never picked, never fails). weight biases an UNDIRECTED hit toward it.
struct SubsystemDef {
    float hp{0.f};
    float weight{1.f};
};

// A fixed-size table indexed by Subsystem. Absent from a DamageDef ⇒ the pre-#675 3-level model.
struct SubsystemSet {
    std::array<SubsystemDef, kSubsystemCount> parts{};

    [[nodiscard]] const SubsystemDef& operator[](Subsystem s) const noexcept {
        return parts[static_cast<int>(s)];
    }
    [[nodiscard]] bool any() const noexcept {
        for (const SubsystemDef& p : parts)
            if (p.hp > 0.f)
                return true;
        return false;
    }
};

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
    std::optional<SubsystemSet> subsystems; // #675; absent = the 3-level model above is the whole story
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
