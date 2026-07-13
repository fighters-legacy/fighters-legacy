// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/DamageDef.h"
#include "entity/ObjectCategory.h"
#include "entity/SignatureDef.h"

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

    // ── sensing (#680) ───────────────────────────────────────────────────────
    // What the entity looks like to an observer. Defaults are the baseline fighter (all 1.0), so an
    // entity that says nothing is exactly as detectable as the numbers in a sensor def assume.
    SignatureDef signatures{};

    // Which sensors the entity carries, as sensor-def ids (e.g. "fl-base:apg63"). Plain strings, so
    // engine-entity does not depend on engine-sensor; ids are resolved against the AssetManager at
    // load time, where an unknown one is a WARNING, not a parse error — a pack's cross-references
    // resolve after all its files are read, and a missing sensor should not stop an aircraft from
    // loading with the rest of its suite.
    //
    // EMPTY IS MEANINGFUL: an AI-controlled entity with no declared sensors gets the builtin
    // eyeball, not omniscience and not blindness (2026-07-12 decision record). Honest sensing is the
    // default; a pack cannot opt out of it by leaving this list off.
    std::vector<std::string> sensorIds;

    // Per-unit acquisition tuning. Absent = the engine default (AiTuning{}), so authors tune only
    // the units they care about; an elite interceptor and a conscript SAM crew can fly identical
    // hardware and still behave differently.
    std::optional<AiTuning> aiTuning;
};

} // namespace fl
