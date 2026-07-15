// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "flight/Geodetic.h" // kEarthRadiusM

#include <string>
#include <vector>

namespace fl {

struct Mission;
class EntityManager;
class FactionRegistry;
class WeatherController;

// A joinable player slot: a mission object marked `player: true`. applyMission does NOT spawn it as a
// world entity — the connect handshake assigns a pilot to an open slot (faction/spawn/type) at connect
// time (#854). Faction 0 = neutral (side not resolvable).
struct PlayerSlot {
    std::string type; // aircraft type id the pilot spawns as
    uint16_t factionIndex{0};
    double pos[3]{};
    float headingDeg{0.f};
};

struct MissionSetupResult {
    std::vector<EntityId> spawned;       // world entities created (excludes player slots)
    std::vector<PlayerSlot> playerSlots; // joinable slots for the handshake (#854)
    std::vector<std::string> warnings;   // e.g. an object whose type failed to spawn
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
                                WeatherController* weather = nullptr, double planetRadiusM = kEarthRadiusM);

} // namespace fl
