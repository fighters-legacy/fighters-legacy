// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "world/FormationRegistry.h"

#include <cstdint>
#include <vector>

// Aggregated world-state surface (#600, Epic M #589) — the shared core.
//
// A structured, deterministic, off-thread-friendly view of the whole battlespace, assembled from a
// cheap sim-thread copy of published entity state at ~1 Hz. Designed ONCE for two consumers: the
// Epic M agentic-AI world-state read API (JSON snapshot + event stream over an out-of-band socket,
// built later on top of this struct) AND the game-master overview map (#861 — the FIRST consumer,
// which cannot use per-camera queryRadius interest because a 128-player map would flood).
//
// This header is a plain-data view: no ENet, no SDL, no JSON. `buildWorldStateSnapshot` is a pure
// function over the sim's entity/formation/peer state, deterministic given a fixed entity set
// (golden-JSON-friendly), so it joins the SDL/ENet-free unit-test set. The sim thread's only cost is
// the bounded copy; any expensive serialization (JSON, wire encode) happens off-thread from the
// result. See docs/ai-architecture.md §3 and docs/architecture.md.

namespace fl {

// Per-entity flags in WorldStateEntity::flags.
enum WorldStateEntityFlag : uint8_t {
    kWorldStatePlayerOwned = 0x01, // flown by a connected peer (ownerPeerId is that peer)
    kWorldStateEcmActive = 0x02,   // ECM/noise jamming on (#529)
};

// One live entity, cheaply copied from EntityState + resolved through the type/formation registries.
struct WorldStateEntity {
    uint32_t entityIdx{0};    // pool index
    uint16_t gen{0};          // generation (with entityIdx, a stable {idx,gen} handle)
    uint16_t factionIndex{0}; // FactionRegistry index (0 = neutral)
    uint32_t typeIndex{0};    // EntityTypeRegistry index
    uint32_t ownerPeerId{0};  // controlling peer id; 0 = server / AI (kNoOwningPeer semantics)
    uint16_t formationId{0};  // FormationRegistry id, kNoFormation (0) = not in a formation
    uint8_t category{0};      // ObjectCategory ordinal (AirVehicle/GroundVehicle/... from the def)
    uint8_t damageLevel{0};   // DamageLevel ordinal
    uint8_t flags{0};         // WorldStateEntityFlag bits
    double pos[3]{};          // world position (m)
    float vel[3]{};           // world velocity (m/s)
    float hpFrac{0.f};        // hp / maxHp in [0,1] (0 when maxHp == 0)
};

// One connected peer's summary (the "peer picture" the snapshot carries alongside entities).
struct WorldStatePeer {
    uint32_t peerId{0};
    uint16_t factionIndex{0}; // the peer's faction (from its entity), 0xFFFF = none
    uint16_t delayTicks{0};   // estimated one-way delay (the per-peer latency class)
    uint8_t role{0};          // PeerRole ordinal (Pilot/Observer)
};

// The full aggregated snapshot at a tick.
struct WorldStateSnapshot {
    uint64_t tick{0};
    std::vector<WorldStateEntity> entities; // ascending entityIdx (deterministic)
    std::vector<WorldStatePeer> peers;      // ascending peerId (deterministic)
    uint8_t weatherPreset{0};               // WeatherPreset ordinal
    float timeOfDayHours{12.f};
};

// Build the snapshot. Iterates entityManager.forEach in ascending pool order (deterministic given a
// fixed entity set), skipping dead entities; resolves `category` from the type registry and
// `formationId` from the (optional) formation registry. `peers` is sorted ascending by peerId for
// determinism. Pure — no I/O, no wire, no time source; the caller supplies tick/weather/peers.
[[nodiscard]] WorldStateSnapshot buildWorldStateSnapshot(uint64_t tick, const EntityManager& entityManager,
                                                         const EntityTypeRegistry& registry,
                                                         const FormationRegistry* formations,
                                                         std::vector<WorldStatePeer> peers, uint8_t weatherPreset,
                                                         float timeOfDayHours);

} // namespace fl
