// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fl {

// Fuel (#862) is the drop-tank store: it mounts on a station like any other store and costs the
// airframe mass + drag, but it is inert -- never fired, never a firing station, never a projectile.
// Inert-ness is a property of the WEAPON's type (stations have no kind of their own; a station's
// allowed list is its whole compatibility contract). FireControl skips it,
// registerProjectileEntityDefs skips it; only the loadout mass/drag accounting reads it.
enum class WeaponType : uint8_t { Missile, Bomb, Rocket, Gun, Pod, Fuel };

// An INERT store (#862) — a `fuel` drop tank or a `pod` (ECM/targeting/recon) — has no reach, no
// seeker and no warhead, only mass and drag. It is carried and it is jettisonable, but it is never
// a firing station.
//
// Inert-ness is the WEAPON's kind, not the station's: the same wet pylon can offer a bomb or a tank,
// and only the mounted store decides. The rule was stated four times — three loadout builders and
// the def parser, which relaxes [performance]/[warhead] on exactly this predicate — so a new inert
// kind would have had to be remembered in four places to be carried without being fireable (#1265).
[[nodiscard]] constexpr bool isInertStore(WeaponType t) noexcept {
    return t == WeaponType::Fuel || t == WeaponType::Pod;
}

enum class WeaponCategory : uint8_t { AirToAir, AirToGround, AirToSea, AntiRadiation };

// How a weapon finds its target. Authored as [seeker] (self-guided) or [guidance] (externally
// guided); both parse into this one struct because they answer the same question.
//
// THE SEEKER HEAD IS A SENSOR (2026-07-14 decision record, executing the migration the 2026-07-12
// record named): `sensorId` references a sensor def, and the missile evaluates it through the same
// Detection.h math as every other observer — one vocabulary, three consumers. What stays HERE is
// what is about the WEAPON rather than the sensor: employment doctrine (fire-and-forget, designator
// support, pitbull range) and trajectory shaping (loft). The legacy fov_deg/acquisition_nm lobe
// fields still parse for one release so packs authored against the pre-#583 schema keep loading;
// the engine and validate-weapon both warn on them.
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
    std::string sensorId;           // sensor-def id for the seeker head, e.g. "fl-base:aim9p-seeker"
    bool fireAndForget{false};      // false = launch platform must keep supporting the shot
    bool requiresDesignator{false}; // laser/GPS: someone must hold the spot
    float pitbullRangeM{0.f};       // ARH: range-to-go at which the missile's own radar goes
                                    // active (and starts EMITTING); 0 = active off the rail
    float loftBiasDeg{0.f};         // climb bias flown while range-to-go > loftRangeM; 0 = no loft
    float loftRangeM{0.f};          // where the loft phase ends and the seeker flies pure PN

    // ── DEPRECATED (one release) ─────────────────────────────────────────────
    // The pre-#583 ad-hoc lobe. Used only when sensorId is empty, to synthesize a seeker lobe so
    // old packs keep flying. Author sensor_id instead.
    float fovDeg{0.f};            // seeker gimbal half-angle; 0 = no seeker lobe
    float acquisitionRangeM{0.f}; // range at which the seeker can take a lock

    [[nodiscard]] bool usesLegacyLobe() const noexcept {
        return sensorId.empty() && (fovDeg > 0.f || acquisitionRangeM > 0.f);
    }
};

struct WeaponPerformance {
    float maxRangeM{0.f};
    float minRangeM{0.f};
    float maxSpeedMps{0.f};
    float motorBurnTimeS{0.f}; // 0 = unpowered (bombs)
    float maxG{0.f};           // 0 = unmanoeuvring
    float cepM{0.f};           // circular error probable; 0 = not specified
    float rateOfFireRpm{0.f};  // guns: rounds per minute; 0 = the engine default
};

struct WarheadDef {
    float blastRadiusM{0.f};
    float damage{0.f};
    bool nuclear{false}; // gates the nuclear effects path (#356): EMP, flash, mushroom cloud
    float yieldKt{0.f};  // required when nuclear; scales the effect radii
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
    uint16_t rounds{0};    // shots this load provides: a gun's magazine, a rail's missile count.
                           // 0 = the engine default (guns 500, everything else 1)
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

    // ASSET NAME (bare stem incl. its own subdirectory, like every other mesh field) for the
    // projectile's in-flight visual. Empty = the builtin placeholder. Only meaningful for weapon
    // types that fly as entities (missile/rocket/bomb); guns are hitscan and never render one.
    std::string mesh;

    std::optional<SeekerDef> seeker; // absent = ballistic/unguided
    WeaponPerformance performance;
    WarheadDef warhead;
    CountermeasureSusceptibility countermeasures;
    WeaponLoad load;
};

} // namespace fl
