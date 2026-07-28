// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/WorldAiProviderHost.h"

#include "dynlib/DynLib.h"
#include "world/NullAiProvider.h"

#include <ILogger.h>

#include <algorithm>
#include <cmath>

namespace fl {

const char* worldAiCapabilityName(WorldAiCapability c) noexcept {
    switch (c) {
    case WorldAiCapability::Mission:
        return "mission";
    case WorldAiCapability::WorldEvolution:
        return "world_evolution";
    case WorldAiCapability::Narrative:
        return "narrative";
    case WorldAiCapability::FactionDecision:
        return "faction_decision";
    case WorldAiCapability::Intent:
        return "intent";
    case WorldAiCapability::Count:
        break;
    }
    return "unknown";
}

namespace {

[[nodiscard]] bool isFiniteVec3(const double v[3]) noexcept {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

[[nodiscard]] bool isAlertLevelOrdinal(AlertLevel l) noexcept {
    return static_cast<uint8_t>(l) <= static_cast<uint8_t>(AlertLevel::WarState);
}

} // namespace

WorldEvolutionResult applyWorldEvolution(const WorldEvolutionDelta& delta,
                                         const std::vector<std::string>& allowedEntityTypes,
                                         const WorldEvolutionSinks& sinks) {
    WorldEvolutionResult r;
    const uint16_t factions = sinks.factionCount ? sinks.factionCount() : 0;

    auto reject = [&r](std::string why) {
        ++r.rejected;
        r.rejections.push_back(std::move(why));
    };
    // A faction index is the field most likely to be wrong: a model counting coalitions from a
    // prompt has no reason to land on the registry's indices, and an out-of-range one applied
    // blindly would either do nothing or hit the wrong side.
    auto validFaction = [&](uint16_t idx) { return factions != 0 && idx < factions; };

    for (const AlertLevelChange& c : delta.alertChanges) {
        if (!validFaction(c.factionIndex))
            reject("alert_level: faction index " + std::to_string(c.factionIndex) + " out of range");
        else if (!isAlertLevelOrdinal(c.level))
            reject("alert_level: level ordinal out of range");
        else if (!sinks.setAlertLevel)
            reject("alert_level: no alert system configured");
        else if (!sinks.setAlertLevel(c.factionIndex, c.level))
            reject("alert_level: refused for faction " + std::to_string(c.factionIndex));
        else
            ++r.applied;
    }

    for (const RelationshipChange& c : delta.relationshipChanges) {
        if (!validFaction(c.factionA) || !validFaction(c.factionB))
            reject("relationship: faction index out of range");
        else if (c.factionA == c.factionB)
            // The registry seeds the diagonal Friendly and a side is not at war with itself; letting
            // a model rewrite it would be a way to make everyone hostile to their own team.
            reject("relationship: a faction's relationship with itself is not settable");
        else if (!sinks.setRelationship)
            reject("relationship: no faction registry configured");
        else if (!sinks.setRelationship(c.factionA, c.factionB, c.relation))
            reject("relationship: refused");
        else
            ++r.applied;
    }

    for (const ZoneControlChange& c : delta.zoneChanges) {
        if (c.zoneId.empty())
            reject("zone_control: empty zone id");
        else if (!validFaction(c.newOwnerFactionIndex))
            reject("zone_control: faction index " + std::to_string(c.newOwnerFactionIndex) + " out of range");
        else if (!sinks.setZoneOwner)
            reject("zone_control: no zone system configured");
        else if (!sinks.setZoneOwner(c.zoneId, c.newOwnerFactionIndex))
            reject("zone_control: unknown zone '" + c.zoneId + "'");
        else
            ++r.applied;
    }

    for (const SpawnEvent& ev : delta.spawnEvents) {
        // The vocabulary gate. A model that invents an entity type must not get to discover whether
        // the server happens to have one by that name — the allowlist IS what it was shown.
        const bool known = std::find(allowedEntityTypes.begin(), allowedEntityTypes.end(), ev.entityTypeId) !=
                           allowedEntityTypes.end();
        if (!known)
            reject("spawn: entity type '" + ev.entityTypeId + "' is not in the advertised vocabulary");
        else if (!validFaction(ev.factionIndex))
            reject("spawn: faction index " + std::to_string(ev.factionIndex) + " out of range");
        else if (!isFiniteVec3(ev.worldPos) || !std::isfinite(ev.headingDeg))
            // A NaN reaching the sim propagates into a transform and then into everything that reads
            // it; the quantizer clamps, but the entity is already lost by then.
            reject("spawn: non-finite position or heading");
        else if (!sinks.spawn)
            reject("spawn: no spawn sink configured");
        else if (!sinks.spawn(ev))
            reject("spawn: refused for '" + ev.entityTypeId + "'");
        else
            ++r.applied;
    }

    return r;
}

std::unique_ptr<IWorldAiProvider> loadWorldAiProvider(const std::string& pluginPath, ILogger& logger,
                                                      bool& loadFailed) {
    loadFailed = false;
    if (pluginPath.empty())
        return std::make_unique<NullAiProvider>();

    void* sym = loadLibrarySymbol(pluginPath, IWorldAiProvider::kFactorySymbol, logger);
    if (!sym) {
        // Configured and unloadable is a REAL failure, distinct from "not configured". A server that
        // quietly ran scripted content because a path was mistyped is the outcome this reports.
        loadFailed = true;
        return std::make_unique<NullAiProvider>();
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto factory = reinterpret_cast<WorldAiProviderFactory>(sym);
    IWorldAiProvider* raw = factory();
    if (!raw) {
        logger.log(LogLevel::Error, __FILE__, __LINE__,
                   ("ai_provider: plugin '" + pluginPath + "' factory returned null").c_str());
        loadFailed = true;
        return std::make_unique<NullAiProvider>();
    }
    return std::unique_ptr<IWorldAiProvider>(raw);
}

} // namespace fl
