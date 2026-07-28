// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/IWorldAiProvider.h"

#include <functional>
#include <memory>
#include <string>

// Loading a provider, and applying what it says (#163).
//
// Two halves, both deliberately separate from the interface itself so they are testable without a
// plugin on disk:
//
//   loadWorldAiProvider  — dlopen a plugin through the shared DynLib, or hand back NullAiProvider
//   applyWorldEvolution  — turn a WorldEvolutionDelta into calls on the live world
//
// applyWorldEvolution is the interesting one. A delta arrives from a MODEL, which means every field
// in it is a suggestion from something that may be confused, out of date, or being manipulated
// through the player chat it was shown. So it validates each change against the world before making
// it, drops the ones that do not hold, and REPORTS what it dropped — a delta that silently
// half-applies is indistinguishable from one that worked.

namespace fl {

class ILogger;

// The sinks the caller wires to the real world. Any of them may be null; a change whose sink is
// missing is counted as rejected rather than silently ignored, so "nothing happened" and "nothing
// was wired" are distinguishable in the result.
struct WorldEvolutionSinks {
    // #162's AlertSystem::setAlertLevel.
    std::function<bool(uint16_t factionIndex, AlertLevel level)> setAlertLevel;
    // FactionRegistry::setRelationship.
    std::function<bool(uint16_t a, uint16_t b, FactionRelation rel)> setRelationship;
    // Zone ownership; false when the zone id is unknown.
    std::function<bool(const std::string& zoneId, uint16_t newOwner)> setZoneOwner;
    // EntityManager::spawn. False when the type is unknown or the spawn failed.
    std::function<bool(const SpawnEvent& ev)> spawn;
    // Faction count, for bounds-checking indices a model supplied. 0 = no registry configured, in
    // which case every faction-indexed change is rejected rather than applied against nothing.
    std::function<uint16_t()> factionCount;
};

struct WorldEvolutionResult {
    int applied{0};
    int rejected{0};
    std::vector<std::string> rejections; // one line per dropped change, with the reason
};

// Apply what holds, drop what does not, and say which was which.
//
// `allowedEntityTypes` is the spawn vocabulary the context advertised. A spawn naming anything else
// is rejected — a model that invents an entity type must not get to find out whether the server
// happens to have one by that name.
[[nodiscard]] WorldEvolutionResult applyWorldEvolution(const WorldEvolutionDelta& delta,
                                                       const std::vector<std::string>& allowedEntityTypes,
                                                       const WorldEvolutionSinks& sinks);

// Load a provider plugin. Returns NullAiProvider when `pluginPath` is empty or the load fails — the
// caller always gets a usable provider and asks supports() rather than checking for null, so there
// is one degradation path instead of two.
//
// `loadFailed` reports whether a configured plugin could not be loaded, so a server can log that
// loudly: silently running scripted content because a path was mistyped is the failure mode this
// distinction exists to prevent.
[[nodiscard]] std::unique_ptr<IWorldAiProvider> loadWorldAiProvider(const std::string& pluginPath, ILogger& logger,
                                                                    bool& loadFailed);

} // namespace fl
