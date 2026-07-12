// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/DamageDef.h"
#include "entity/ObjectCategory.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fl {

// What a hardpoint can carry. Matches the `type` field of a weapon def, plus the non-weapon
// stores (fuel tanks, pods) that occupy a station without being one.
enum class HardpointType : uint8_t { Missile, Bomb, Rocket, Gun, Fuel, Pod };

// One weapon station on an airframe. Hardpoints are a property of the ENTITY, not of its flight
// model: the flight model is aerodynamics, and what the aircraft is allowed to carry is not. The
// physics consequence of a loadout reaches the flight model through PayloadEffect
// (engine/flight/AeroForces.h), which is the only coupling that should exist.
struct Hardpoint {
    int slot{0}; // station number; unique within an entity
    HardpointType type{HardpointType::Missile};
    std::vector<std::string> allowed; // weapon ids this station accepts; never empty
    std::string defaultWeapon;        // pre-loaded weapon id; must be a member of `allowed`
};

// Immutable definition for one entity type, loaded from a content pack TOML file and
// registered with EntityTypeRegistry. Shared by all live instances of the same type.
struct EntityDef {
    std::string id; // content-pack-scoped, e.g. "fl-base:f15c"
    std::string name;
    ObjectCategory category{ObjectCategory::AirVehicle};
    float maxHp{100.f};
    std::optional<DamageDef> damage;   // absent = binary death (no progressive damage)
    std::string mesh;                  // asset name for primary geometry
    std::string classicDamageMesh;     // JumpToDamage geometry variant; empty if none
    std::string flightModelId;         // flight-model asset id; empty = builtin UFO model (server-side only)
    std::string aiScriptId;            // Lua AI script asset name; empty = no scripted AI (server-side only)
    std::vector<Hardpoint> hardpoints; // weapon stations; empty = carries nothing
};

} // namespace fl
