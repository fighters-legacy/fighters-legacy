// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "world/AlertLevel.h"
#include "world/FactionDef.h"
#include "world/FactionRegistry.h"
#include "world/FormationRegistry.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
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
// result. See docs/developer/ai-architecture.md §3 and docs/developer/architecture.md.

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
    // Current wing sweep in DEGREES (#1195), 0 for anything without a [wing_sweep] table. Filled by
    // WorldBroadcaster from the entity's flight integrator, not by buildWorldStateSnapshot — which
    // sees only the entity pool and stays pure.
    //
    // It is here because until #1195 the angle was readable NOWHERE outside the integrator: not on
    // the Lua state table, not in --mission-report, not in the replay, not from any console command,
    // so "does this aircraft's sweep follow its Mach schedule" could only be answered by an
    // in-process C++ test. This is the headless answer, and it is degrees rather than the 0..1 wire
    // fraction because a reader wants to compare it against the schedule the model publishes.
    float sweepDeg{0.f};
};

// One connected peer's summary (the "peer picture" the snapshot carries alongside entities).
struct WorldStatePeer {
    uint32_t peerId{0};
    uint16_t factionIndex{0}; // the peer's faction (from its entity), 0xFFFF = none
    uint16_t delayTicks{0};   // estimated one-way delay (the per-peer latency class)
    uint8_t role{0};          // PeerRole ordinal (Pilot/Observer)
};

// One coalition, with the posture and relationships an agent needs to reason about sides (#600).
// Without these the snapshot said which faction index an entity belonged to but nothing about what
// that index MEANT -- who it was at war with, and whether its airspace was hot.
struct WorldStateFaction {
    uint16_t factionIndex{0};
    std::string id;
    std::string name;
    uint8_t alertLevel{0}; // AlertLevel ordinal (#162)
};

// Mission / objective state. Filled by the host, because engine-net does not link engine-mission --
// the mission runtime pushes this in rather than the snapshot reaching out for it.
struct WorldStateMission {
    bool active{false};
    std::string name;
    uint8_t outcome{0}; // MissionResultCode ordinal: 0 incomplete, 1 success, 2 failure
    uint32_t triggersFired{0};
    double elapsedSeconds{0.0};
};

// Environment inputs, bundled so the build signature does not grow a scalar per weather field.
struct WorldStateEnvironment {
    uint8_t weatherPreset{0}; // WeatherPreset ordinal
    float timeOfDayHours{12.f};
    float windX{0.f}; // world-frame steady wind (m/s); an agent planning a strike needs it, and it
    float windZ{0.f}; // was already server-authoritative and already broadcast to clients
};

// The full aggregated snapshot at a tick.
struct WorldStateSnapshot {
    uint64_t tick{0};
    std::vector<WorldStateEntity> entities;  // ascending entityIdx (deterministic)
    std::vector<WorldStatePeer> peers;       // ascending peerId (deterministic)
    std::vector<WorldStateFaction> factions; // ascending factionIndex (deterministic)
    // Row-major factions.size() x factions.size() FactionRelation ordinals; empty when no registry.
    // Stored flat rather than as nested vectors so the JSON emitter walks it in one pass and the
    // determinism story is "one order, no map iteration".
    std::vector<uint8_t> relationships;
    WorldStateMission mission;
    uint8_t weatherPreset{0}; // WeatherPreset ordinal
    float timeOfDayHours{12.f};
    float windX{0.f};
    float windZ{0.f};

    // Relationship between two faction indices; Neutral when either is out of range.
    [[nodiscard]] FactionRelation relationship(uint16_t a, uint16_t b) const noexcept {
        const std::size_t n = factions.size();
        if (a >= n || b >= n || relationships.size() != n * n)
            return FactionRelation::Neutral;
        return static_cast<FactionRelation>(relationships[static_cast<std::size_t>(a) * n + b]);
    }
};

// Build the snapshot. Iterates entityManager.forEach in ascending pool order (deterministic given a
// fixed entity set), skipping dead entities; resolves `category` from the type registry and
// `formationId` from the (optional) formation registry. `peers` is sorted ascending by peerId for
// determinism, and the faction table is emitted in ascending index order for the same reason. Pure —
// no I/O, no wire, no time source; the caller supplies tick, environment, peers and mission state.
//
// `factions` and `mission` are both optional (null = omit that block), so a sandbox server with no
// mission and no coalitions produces a valid, smaller snapshot rather than a padded one.
[[nodiscard]] WorldStateSnapshot
buildWorldStateSnapshot(uint64_t tick, const EntityManager& entityManager, const EntityTypeRegistry& registry,
                        const FormationRegistry* formations, const FactionRegistry* factions,
                        std::vector<WorldStatePeer> peers, const WorldStateEnvironment& env,
                        const WorldStateMission* mission);

// Off-thread publication of the latest snapshot (#600, D4).
//
// worldState() hands back a const& to a member the sim thread rebuilds in place, which is fine for a
// sim-thread reader (the GM feed) and a data race for anything else. REST, MCP, the replay recorder
// and the AI provider all want to read it from their own threads, so the sim PUBLISHES an immutable
// snapshot and readers take a shared_ptr to whatever was current when they asked.
//
// A reader's snapshot therefore stays valid and unchanging for as long as it holds the pointer, even
// as the sim publishes newer ones -- which is the property a JSON serializer needs, since it cannot
// hold a lock for the length of a multi-thousand-entity document.
//
// Deliberately a mutex rather than std::atomic<std::shared_ptr>: publication happens at ~1 Hz, so
// there is no contention to optimise away, and this is the codebase's existing cross-thread idiom
// (ServerQueryResponder, TickProfiler::snapshot). A lock-free version here would be the repo's first
// atomic shared_ptr and would buy nothing measurable.
class WorldStatePublisher {
  public:
    // Sim thread. Takes ownership; the previous snapshot stays alive until its last reader drops it.
    void publish(std::shared_ptr<const WorldStateSnapshot> snap) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_current = std::move(snap);
    }

    // Any thread. Null until the first publish -- callers must handle that rather than assume a
    // server has been up long enough to have produced one.
    [[nodiscard]] std::shared_ptr<const WorldStateSnapshot> get() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_current;
    }

  private:
    mutable std::mutex m_mutex;
    std::shared_ptr<const WorldStateSnapshot> m_current;
};

} // namespace fl
