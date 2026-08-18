// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "flight/Geodetic.h" // kEarthRadiusM

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fl {

struct Mission;
struct MissionObject;
class EntityManager;
class FactionRegistry;
class WeatherController;

// Called once per spawned (non-player) mission object, with its EntityId and the source object. The
// caller (fl-server) uses it to attach a controller from `ai`/`route` and apply a `loadout:` override
// (#855) — engine-mission does not link engine-ai / the weapon registry, so the seam lives here.
using MissionSpawnHook = std::function<void(EntityId, const MissionObject&)>;

// Resolves the terrain/ground elevation (MSL metres) under a WORLD POSITION for a `start: ground`
// object (#885), so it spawns sitting ON the ground rather than at an authored altitude. The object
// is then placed at that altitude along its own radial, which is what "on the ground" means anywhere
// on the sphere (#1211 — the (x, z) column form this used to take only located a place near the
// world origin). The caller (fl-server) supplies the terrain heightAt; engine-mission has no terrain
// of its own. Empty = ground starts keep their authored altitude (used by unit tests with no terrain).
using GroundHeightFn = std::function<double(double x, double y, double z)>;

// A joinable player slot: a mission object marked `player: true`. applyMission does NOT spawn it as a
// world entity — the connect handshake assigns a pilot to an open slot (faction/spawn/type) at connect
// time (#854). Faction 0 = neutral (side not resolvable).
struct PlayerSlot {
    std::string id;   // the mission object id (#884), so the objective evaluator can bind the pilot's
                      // spawned aircraft to destroy(<id>) once a peer claims the slot
    std::string type; // aircraft type id the pilot spawns as
    uint16_t factionIndex{0};
    double pos[3]{};
    float headingDeg{0.f};
    float quat[4]{0.f, 0.f, 0.f, 1.f}; // resolved spawn orientation (heading on the local tangent frame)
    std::optional<float> speed;        // initial airspeed (m/s); absent = a cruise default for the pilot (#883)
    // Per-station stores the pilot takes off with (#1209), same vocabulary as MissionObject::loadout
    // ("~"/"-"/"" = an empty station; fewer entries than stations keeps the rest at their defaults).
    // Empty = the entity def's default fit. It is a FIXED fit, not a suggestion: nothing in the
    // engine lets a pilot change stores after the fact, so a mission that asks for guns-only gets
    // guns-only. Applied by the caller when the pilot is assigned to the slot, not at parse time,
    // because it needs the weapon registry (which engine-mission does not link).
    std::vector<std::string> loadout;
};

struct MissionSetupResult {
    std::vector<EntityId> spawned;       // world entities created (excludes player slots)
    std::vector<PlayerSlot> playerSlots; // joinable slots for the handshake (#854)
    std::vector<std::string> warnings;   // e.g. an object whose type failed to spawn
    // Mission object id -> spawned EntityId, for the objective/trigger evaluator's destroy(<id>)
    // predicate (#633). Only successfully-spawned non-player objects appear here.
    std::vector<std::pair<std::string, EntityId>> objectEntities;
};

// Sets up the sim from a parsed mission: loads the coalition registry (index 0 reserved neutral, real
// sides 1..N, distinct non-allied sides Hostile / allies Friendly), spawns every non-player object at
// its position + heading with its faction, and applies the mission's weather/time/wind to `weather`
// (when supplied). Player-slot objects are returned in `playerSlots` rather than spawned.
//
// `factions` is an OUT-PARAMETER the caller owns (the registry holds a std::mutex and is non-movable,
// so it cannot be returned by value; single-owner-by-reference is its documented contract). The caller
// keeps it alive for the sim's lifetime and hands it to WorldBroadcaster::setFactionRegistry.
//
// `planetRadiusM` places headings on the local tangent frame at each spawn (correct anywhere on the
// sphere); default Earth. Call before gameLoop.start() (spawn/registry setup is pre-start only).
MissionSetupResult applyMission(const Mission& mission, EntityManager& em, FactionRegistry& factions,
                                WeatherController* weather = nullptr, double planetRadiusM = kEarthRadiusM,
                                const MissionSpawnHook& onSpawned = {}, const GroundHeightFn& groundHeight = {});

} // namespace fl
