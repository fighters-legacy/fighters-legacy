// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/WorldState.h"

#include "entity/EntityDef.h" // ObjectCategory (resolved from the type registry)
#include "world/FactionRegistry.h"

#include <algorithm>

namespace fl {

WorldStateSnapshot buildWorldStateSnapshot(uint64_t tick, const EntityManager& entityManager,
                                           const EntityTypeRegistry& registry, const FormationRegistry* formations,
                                           const FactionRegistry* factions, std::vector<WorldStatePeer> peers,
                                           const WorldStateEnvironment& env, const WorldStateMission* mission) {
    WorldStateSnapshot snap;
    snap.tick = tick;
    snap.weatherPreset = env.weatherPreset;
    snap.timeOfDayHours = env.timeOfDayHours;
    snap.windX = env.windX;
    snap.windZ = env.windZ;
    if (mission)
        snap.mission = *mission;

    // forEach walks pool slots in ascending index order, so the entity list is deterministic given a
    // fixed entity set (the golden-snapshot property #600 requires).
    entityManager.forEach([&](const EntityState& s) {
        if (s.dead)
            return;
        WorldStateEntity e;
        e.entityIdx = s.id.index;
        e.gen = static_cast<uint16_t>(s.id.generation);
        e.factionIndex = s.factionIndex;
        e.typeIndex = s.typeIndex;
        e.ownerPeerId = s.ownerId;
        e.formationId = formations ? formations->formationOfEntity(s.id) : kNoFormation;
        if (const EntityDef* def = registry.byIndex(s.typeIndex))
            e.category = static_cast<uint8_t>(def->category);
        e.damageLevel = static_cast<uint8_t>(s.damageLevel);
        e.flags = 0;
        if (s.playerOwned)
            e.flags |= kWorldStatePlayerOwned;
        if (s.ecmActive)
            e.flags |= kWorldStateEcmActive;
        e.pos[0] = s.transform.pos[0];
        e.pos[1] = s.transform.pos[1];
        e.pos[2] = s.transform.pos[2];
        e.vel[0] = s.transform.vel[0];
        e.vel[1] = s.transform.vel[1];
        e.vel[2] = s.transform.vel[2];
        e.hpFrac = (s.maxHp > 0.f) ? std::clamp(s.hp / s.maxHp, 0.f, 1.f) : 0.f;
        snap.entities.push_back(e);
    });

    // Faction table + relationship matrix (#600). Walked by ascending index rather than by iterating
    // any map, so the emitted order is the same on every run and every platform.
    if (factions) {
        const uint16_t n = factions->count();
        snap.factions.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            WorldStateFaction f;
            f.factionIndex = i;
            if (const FactionDef* def = factions->get(i)) {
                f.id = def->id;
                f.name = def->name;
            }
            f.alertLevel = static_cast<uint8_t>(factions->alertLevel(i));
            snap.factions.push_back(std::move(f));
        }
        snap.relationships.resize(static_cast<std::size_t>(n) * n);
        for (uint16_t a = 0; a < n; ++a)
            for (uint16_t b = 0; b < n; ++b)
                snap.relationships[static_cast<std::size_t>(a) * n + b] =
                    static_cast<uint8_t>(factions->relationship(a, b));
    }

    std::sort(peers.begin(), peers.end(),
              [](const WorldStatePeer& a, const WorldStatePeer& b) { return a.peerId < b.peerId; });
    snap.peers = std::move(peers);
    return snap;
}

} // namespace fl
