// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fl {

enum class WeaponType : uint8_t { Missile, Bomb, Rocket, Gun, Pod };

enum class WeaponCategory : uint8_t { AirToAir, AirToGround, AirToSea, AntiRadiation };

// How a weapon finds its target. Authored as [seeker] (self-guided) or [guidance] (externally
// guided); both parse into this one struct because they answer the same question.
//
// PROVISIONAL: the 2026-07-12 sensor decision record (docs/architecture.md) folds this into the
// shared SensorDef vocabulary — `sensorId` will replace the ad-hoc lobe fields under #583. Authored
// content stays valid across that migration; the field names below do not.
enum class SeekerType : uint8_t {
    ActiveRadar,
    SemiActiveRadar,
    Infrared,
    Laser,
    Gps,
    AntiRadiation,
    Unguided,
};

struct SeekerDef {
    SeekerType type{SeekerType::Unguided};
    float fovDeg{0.f};              // seeker gimbal half-angle; 0 = no seeker lobe
    float acquisitionRangeM{0.f};   // range at which the seeker can take a lock
    bool fireAndForget{false};      // false = launch platform must keep supporting the shot
    bool requiresDesignator{false}; // laser/GPS: someone must hold the spot
};

struct WeaponPerformance {
    float maxRangeM{0.f};
    float minRangeM{0.f};
    float maxSpeedMps{0.f};
    float motorBurnTimeS{0.f}; // 0 = unpowered (bombs)
    float maxG{0.f};           // 0 = unmanoeuvring
    float cepM{0.f};           // circular error probable; 0 = not specified
};

struct WarheadDef {
    float blastRadiusM{0.f};
    float damage{0.f};
};

// Susceptibility values are fractions in [0, 1]: 0 = immune, 1 = always defeated.
struct CountermeasureSusceptibility {
    float chaff{0.f};
    float flare{0.f};
    float notch{0.f}; // defeated by a beam/notch manoeuvre against a doppler seeker
};

// What carrying the weapon costs the airframe. Feeds PayloadEffect (engine/flight/AeroForces.h).
struct WeaponLoad {
    float massKg{0.f};
    float dragFactor{0.f}; // added to the carrier's cd0 while the store is on the rail
};

// Immutable definition for one weapon, loaded from a content pack TOML file.
//
// UNITS ARE SI. Weapon TOML is authored in aviation units (nautical miles, knots, pounds, feet)
// because that is what the source data and the people writing it use; the parser converts on the
// way in, so nothing downstream has to remember which field is imperial.
struct WeaponDef {
    std::string id; // content-pack-scoped, e.g. "fl-base:aim120c"
    std::string name;
    WeaponType type{WeaponType::Missile};
    WeaponCategory category{WeaponCategory::AirToAir};

    std::optional<SeekerDef> seeker; // absent = ballistic/unguided
    WeaponPerformance performance;
    WarheadDef warhead;
    CountermeasureSusceptibility countermeasures;
    WeaponLoad load;
};

} // namespace fl
