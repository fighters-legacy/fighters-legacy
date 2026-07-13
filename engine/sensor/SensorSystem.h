// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/SignatureDef.h"
#include "sensor/Detection.h"
#include "sensor/SensorDef.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {
class EntityManager;
class EntityTypeRegistry;
class SpatialIndex;
struct EntityState;
} // namespace fl

namespace fl::sensor {

// The maximum number of contacts one observer will hold. A real crew does not track a hundred
// things; more importantly this bounds the per-observer work and memory at 128 players, and a cap
// that is never hit is a cap that costs nothing. Contacts beyond it are dropped in ascending-index
// order, deterministically — never randomly, or two workers would disagree.
inline constexpr std::size_t kMaxContactsPerObserver = 32;

// One thing an observer believes is out there.
//
// THIS IS THE ONLY THING A CONSUMER GETS. Not an EntityState, not a position from the
// EntityManager: a contact, with LAST-KNOWN state that may be stale or wrong (2026-07-12 decision
// record). An AI reading these cannot see through terrain or across the map, because ground truth is
// not reachable from here — that is a structural property, not a rule someone has to remember.
struct Contact {
    EntityId id{};            // the target
    uint32_t typeIndex{0};    // its entity type (what the observer believes it is looking at)
    uint16_t factionIndex{0}; //
    ContactState state{ContactState::Lost};
    double lastKnownPos[3]{};      // where it was when last actually seen — NOT where it is now
    float lastKnownVel[3]{};       //
    uint64_t firstDetectedTick{0}; //
    uint64_t lastSeenTick{0};      //

    // Which KINDS of sensor are holding this contact, as a bitmask of (1 << SensorType). A target can
    // be held by two at once (found on radar, also visible to the eyeball), and the difference
    // matters downstream: "he has me on radar" and "he can see me" are not the same tactical fact,
    // and RWR/EMCON (#526/#529) will need exactly this distinction.
    uint8_t sensorTypeMask{0};

    // False until the observer's reaction delay has elapsed since first detection. A contact exists
    // before its owner has reacted to it: seeing is not the same as noticing, and a difficulty knob
    // that made a rookie SEE less would be a lie — what a rookie does is take longer to act.
    bool reacted{false};

    [[nodiscard]] bool held() const noexcept {
        return state != ContactState::Lost;
    }
    // A firing-quality track, as opposed to a bearing you have merely found.
    [[nodiscard]] bool locked() const noexcept {
        return state == ContactState::Locked;
    }
};

// True when `mask` (a Contact::sensorTypeMask) includes `type`.
[[nodiscard]] inline bool holdsSensorType(uint8_t mask, SensorType type) noexcept {
    return (mask & static_cast<uint8_t>(1u << static_cast<int>(type))) != 0;
}

// One observer's view of the world. Ordered by entity index, so iteration is deterministic.
struct ContactTable {
    std::vector<Contact> contacts;

    [[nodiscard]] const Contact* find(EntityId id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return contacts.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return contacts.empty();
    }
    [[nodiscard]] auto begin() const noexcept {
        return contacts.begin();
    }
    [[nodiscard]] auto end() const noexcept {
        return contacts.end();
    }
};

// Everything the sensing pass keeps for one sensor-equipped entity.
struct ObserverState {
    std::vector<std::shared_ptr<const SensorDef>> sensors; // resolved once, at registration
    float skill{0.5f};                                     // EntityDef [ai].skill — 0.5 = unity (see effectivePod)
    float reaction{0.5f};                                  // EntityDef [ai].reaction — 0.5 = unity

    // Is this observer radiating? Radar and laser TRACK lobes require it. Default true; this is the
    // seam EMCON, RWR and SAM radar shutdown hang off (#526/#529), and nothing flips it yet.
    bool emitting{true};

    ContactTable contacts;

    // Per (target, sensor slot) lifecycle state. Keyed by (targetIdx << 32 | slot) so one observer
    // can hold the same target on two sensors at once (found on radar, also visible to the eyeball)
    // without them fighting over one track.
    std::unordered_map<uint64_t, ContactTrack> tracks;

    uint64_t lastCheckTick{0};
    bool everChecked{false};
};

// Per-tick sensing for every sensor-equipped entity.
//
// Owns the observer side-storage (the `m_controlledEntities` idiom — EntityState stays a flat POD
// and knows nothing about being observed). WorldBroadcaster drives it: one call per observer, run
// data-parallel, each writing only its own ObserverState.
//
// SERIAL-EQUIVALENT BY CONSTRUCTION: an observer reads only const world state (EntityState, the
// SpatialIndex frozen for this tick) and writes only its own slot, and every die is seeded from
// (observer, target, tick, slot, lobe) rather than drawn from shared RNG state. So the contact
// tables are byte-identical on one worker and on sixteen — which they must be, or the same server
// would make different decisions depending on how many cores it happened to have.
class SensorSystem {
  public:
    // Resolves a sensor-def id (e.g. "fl-base:apg63") to a parsed def. Injected as a std::function
    // for the same reason setFlightModelResolver is: engine-net must not link engine-content.
    // Returns nullptr for an unknown id — the caller warns once and carries on with the rest of the
    // entity's suite rather than refusing to spawn it.
    using SensorDefResolver = std::function<std::shared_ptr<const SensorDef>(const std::string& id)>;

    SensorSystem(const EntityManager& em, const EntityTypeRegistry& registry);

    void setResolver(SensorDefResolver fn);

    // Registers an observer for a spawned entity. `sensorIds` empty (and `aiControlled`) ⇒ the
    // builtin eyeball, per the decision record: honest sensing is the default, not an opt-in, so an
    // entity that declares no sensors gets eyes rather than omniscience or blindness.
    void addObserver(uint32_t entityIdx, const std::vector<std::string>& sensorIds, float skill, float reaction);
    void removeObserver(uint32_t entityIdx);

    // Sim-thread only.
    void setEmitting(uint32_t entityIdx, bool emitting);
    [[nodiscard]] bool emitting(uint32_t entityIdx) const;

    // The consumer-facing accessor. Null when the entity has no sensors — which a controller must
    // read as "sensing was not evaluated for me", never as "I see nothing" (see AiTickContext).
    [[nodiscard]] const ContactTable* contactsFor(uint32_t entityIdx) const;

    [[nodiscard]] std::size_t observerCount() const noexcept {
        return m_observers.size();
    }

    // Gathers the observers due a geometry check on this tick into a stable, indexable range.
    // Checks are STAGGERED — an observer is due when `(tick + entityIdx) % stride == 0` — so the
    // per-tick cost is spread evenly instead of every sensor in the world firing on the same tick
    // and leaving the other nine idle at 10 Hz.
    struct ObserverWork {
        uint32_t entityIdx{0};
        ObserverState* obs{nullptr};
        const EntityState* state{nullptr};
        float checkDtS{0.f}; // wall seconds since THIS observer's previous check (drives the coast)
    };
    std::vector<ObserverWork>& gatherDue(uint64_t tickIndex, uint32_t strideTicks, double simDt);

    // Evaluates one observer. Called from the parallel region; writes only `work.obs`.
    void evaluateObserver(const ObserverWork& work, const SpatialIndex& si, uint64_t tickIndex,
                          const SensingEnvironment& env, float radarRangeFraction, float reactionTimeS);

    // Reaction-delay bookkeeping for every observer, every tick — it needs no geometry, so it is not
    // staggered: a contact's `reacted` flag must flip on the exact tick the delay elapses, not up to
    // a stride later.
    void updateReactions(uint64_t tickIndex, double simDt, float reactionTimeS);

    // The largest factor by which any entity type in the registry multiplies a sensor's range
    // (sqrt for radar, linear otherwise). The spatial query must be widened by this or a very loud
    // target sitting beyond a sensor's BASELINE range would never be handed to the cone test at all.
    // Computed once from the registry, which is fixed before the loop starts.
    [[nodiscard]] float maxSignatureScale() const noexcept {
        return m_maxSignatureScale;
    }
    void recomputeSignatureScale();

  private:
    const EntityManager& m_entityManager;
    const EntityTypeRegistry& m_registry;
    SensorDefResolver m_resolver;

    std::unordered_map<uint32_t, ObserverState> m_observers;
    std::vector<ObserverWork> m_work; // reused across ticks; no per-tick allocation
    float m_maxSignatureScale{1.f};

    [[nodiscard]] std::shared_ptr<const SensorDef> resolve(const std::string& id) const;
};

} // namespace fl::sensor
