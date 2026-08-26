// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/ObjectCategory.h" // Contact::category — what kind of thing is out there (#1339)
#include "entity/SignatureDef.h"
#include "sensor/Detection.h"
#include "sensor/Iff.h"
#include "sensor/RadarMode.h"
#include "sensor/SensorDef.h"
#include "world/FactionDef.h" // FactionRelation (IFF resolver signature)

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
    EntityId id{};         // the target
    uint32_t typeIndex{0}; // its entity type (what the observer believes it is looking at)
    // What KIND of thing the observer believes it is holding (#1339). Air, ground, naval, structure:
    // the separation a radar operator reads off the scope and an eyeball reads off the horizon, and
    // the first question any employment decision asks — an air-to-air cone geometry cannot be flown
    // against a SAM site, and until this existed a script could not tell the two apart at all.
    //
    // NOT an identification wallhack: `ident` still gates who the contact belongs to, which is the
    // fact that would be cheating. Where the contact is and how it moves is what a sensor measures.
    // Defaults to AirVehicle for a contact whose type could not be resolved, matching the pre-#1339
    // assumption every consumer already made.
    ObjectCategory category{ObjectCategory::AirVehicle};
    uint16_t factionIndex{0}; // the target's ACTUAL faction (ground truth) — see `ident` for what the
                              // observer has actually IDENTIFIED, which is the honest, display-safe fact
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

    // True only for a firing-quality RADAR lock: a Locked contact held by a radar in STT (#526). A
    // Search-mode radar never locks; a TWS radar can reach Locked but its energy is spread across the
    // scan, so `firingQuality` stays false — TWS cannot provide the continuous illumination a SARH
    // shot needs, and a hostile RWR reads a TWS lock as a scan, not a lock tone. A lock held by a
    // non-radar sensor (IRST, eyeball) leaves this false: only the radar has an STT mode.
    bool firingQuality{false};

    // What the observer has IDENTIFIED this contact as (#527): Friend (it squawked), Foe (hostile AND
    // positively identified — VID or a firing-quality lock), or Unknown (detected but not yet ID'd).
    // THIS, not `factionIndex`, is the display-safe fact — handing a bare radar blip's true faction to
    // a pilot would be an identification wallhack. Computed each check from the observer's coalition
    // relationship to the target and how the contact is held. See classifyIff (Iff.h).
    Identification ident{Identification::Unknown};

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

// How hard someone is looking at you (#526 RWR). A radar warning receiver is passive and honest: it
// hears an emitter only when that emitter's beam is actually on you — which is exactly when the
// emitter holds YOU as a contact on a radar/laser sensor while radiating. So a threat warning is the
// INVERSE of a contact, built by reading every emitting observer's table after the sensing pass.
enum class ThreatLevel : uint8_t {
    Search = 0, // an emitter is sweeping/scanning you (Detected, or a soft TWS track): a strobe
    Lock = 1,   // an emitter holds a firing-quality lock on you (STT): a steady lock tone
    Launch = 2, // a radar-guided missile is in the air GUIDING on you (#960): the launch/active tone.
                // The worst level — an inbound weapon, not merely a look. Sourced from live guided
                // projectiles, not the contact tables (see SensorSystem::setMissileThreatProvider).
};

// One emitter your RWR can hear. Position is the EMITTER's world position, so a display can draw the
// bearing to it relative to your own heading; the RWR does not tell you your own aspect for free.
struct ThreatWarning {
    EntityId emitterId{};                  // who is painting you
    uint32_t emitterTypeIndex{0};          // the emitter's entity type (what it appears to be)
    uint16_t emitterFactionIndex{0};       //
    SensorType channel{SensorType::Radar}; // radar or laser (only active emitters announce themselves)
    ThreatLevel level{ThreatLevel::Search};
    double emitterPos[3]{}; // where the emitter is (world m) — the RWR strobe bearing derives from this
};

// A cap keeps the RWR bounded per observer and the wire cost of the datalink (#528) predictable. A
// real threat display shows a handful of the highest-priority emitters, not a hundred; locks outrank
// strobes, nearer outranks farther, and ties break on emitter index — deterministic, like the contact
// cap, so parallel workers never disagree (the RWR pass is serial, but the same discipline holds).
inline constexpr std::size_t kMaxThreatsPerObserver = 16;

// One observer's radar-warning picture. Ordered by emitter index for deterministic iteration.
struct ThreatWarningSet {
    std::vector<ThreatWarning> threats;

    [[nodiscard]] std::size_t size() const noexcept {
        return threats.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return threats.empty();
    }
    [[nodiscard]] auto begin() const noexcept {
        return threats.begin();
    }
    [[nodiscard]] auto end() const noexcept {
        return threats.end();
    }
    // The worst thing looking at you: true when any emitter holds a firing-quality lock OR has a
    // missile guiding on you (a launch is at least a lock in threat terms).
    [[nodiscard]] bool anyLock() const noexcept {
        for (const ThreatWarning& t : threats)
            if (t.level == ThreatLevel::Lock || t.level == ThreatLevel::Launch)
                return true;
        return false;
    }
    // True when a radar-guided missile is in the air guiding on you (#960).
    [[nodiscard]] bool anyLaunch() const noexcept {
        for (const ThreatWarning& t : threats)
            if (t.level == ThreatLevel::Launch)
                return true;
        return false;
    }
    // The highest threat level present (Search default when empty is fine for callers that also
    // check emptiness). Launch > Lock > Search by ordinal.
    [[nodiscard]] ThreatLevel worst() const noexcept {
        ThreatLevel w = ThreatLevel::Search;
        for (const ThreatWarning& t : threats)
            if (static_cast<uint8_t>(t.level) > static_cast<uint8_t>(w))
                w = t.level;
        return w;
    }
};

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

    // Is this observer radiating? Radar and laser lobes require it. Kept in sync with `radarMode`
    // (Silent ⇒ false, any active mode ⇒ true) so the pure detection math has a single bool to gate
    // on, while `radarMode` carries the richer player/AI intent. setAvionicsFailed forces both off.
    bool emitting{true};

    // Radar operating mode (#526). Governs radar-typed sensors only: Silent (EMCON), Search (no
    // lock), TWS (soft multi-track), STT (one hard firing-quality lock on `designatedTarget`).
    //
    // Default TWS, NOT Search — this preserves the pre-#526 behavior where a radar evaluated its track
    // lobe and could reach Locked, which the AI threat conditions and SARH support path already rely
    // on. Search (bearing only) and STT (one firing-quality lock) are the explicit modes a player or
    // AI selects; TWS is the neutral "scanning and holding soft tracks" a radar boots into.
    RadarMode radarMode{RadarMode::Tws};

    // The STT target. Valid only meaningfully in STT mode; in TWS/Search it may still name the
    // player's priority contact for the datalink/HUD but does not change what the radar holds. In STT
    // with no valid designation the radar holds nothing (you must designate first) — the sensing pass
    // auto-designates the nearest hostile so a bare STT is not silently dead.
    EntityId designatedTarget{};

    ContactTable contacts;

    // RWR output (#526): who is painting THIS observer, rebuilt each RWR pass. See ThreatWarningSet.
    ThreatWarningSet threats;

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

    // Resolves the observer→target coalition relationship for IFF (#527). Injected as a std::function
    // for the same reason as SensorDefResolver: engine-sensor must not link engine-world, so the
    // caller (which owns the FactionRegistry) hands the relationship in. Null = the affiliation-rule
    // fallback (affiliationRelation), matching fl::hostile()'s pre-mission behavior.
    using IffResolver = std::function<FactionRelation(uint16_t observerFaction, uint16_t targetFaction)>;

    SensorSystem(const EntityManager& em, const EntityTypeRegistry& registry);

    void setResolver(SensorDefResolver fn);
    void setIffResolver(IffResolver fn);

    // Registers an observer for a spawned entity. `sensorIds` empty (and `aiControlled`) ⇒ the
    // builtin eyeball, per the decision record: honest sensing is the default, not an opt-in, so an
    // entity that declares no sensors gets eyes rather than omniscience or blindness.
    void addObserver(uint32_t entityIdx, const std::vector<std::string>& sensorIds, float skill, float reaction);
    void removeObserver(uint32_t entityIdx);

    // Sim-thread only.
    void setEmitting(uint32_t entityIdx, bool emitting);
    [[nodiscard]] bool emitting(uint32_t entityIdx) const;

    // Radar operating mode + STT designation (#526). Sim-thread only; no-op for a non-observer.
    // setRadarMode keeps `emitting` consistent (Silent ⇒ not radiating). Setting Silent drops radar
    // tracks on the next check honestly rather than freezing a lock the radar no longer holds.
    void setRadarMode(uint32_t entityIdx, RadarMode mode);
    [[nodiscard]] RadarMode radarMode(uint32_t entityIdx) const;
    void setDesignatedTarget(uint32_t entityIdx, EntityId target);
    [[nodiscard]] EntityId designatedTarget(uint32_t entityIdx) const;

    // Critical-damage avionics failure (#626): strips the observer to visual-only (builtin eyeball
    // if nothing remains), stops emissions, and drops every held track — the eyes re-acquire
    // honestly. Sim-thread only; idempotent; no-op for a non-observer.
    void setAvionicsFailed(uint32_t entityIdx);

    // The consumer-facing accessor. Null when the entity has no sensors — which a controller must
    // read as "sensing was not evaluated for me", never as "I see nothing" (see AiTickContext).
    [[nodiscard]] const ContactTable* contactsFor(uint32_t entityIdx) const;

    // The RWR picture for one observer (#526). Null when the entity is not an observer. Empty means
    // "nothing is painting me right now", which — unlike a null contact table — IS a fact the RWR
    // knows: a passive receiver that hears nothing is telling you something.
    [[nodiscard]] const ThreatWarningSet* threatsFor(uint32_t entityIdx) const;

    [[nodiscard]] std::size_t observerCount() const noexcept {
        return m_observers.size();
    }

    // Every observer's entity index, ascending (#528). The datalink builder groups these by faction to
    // fuse a team's shared picture; sorted so the grouping — and therefore the wire — is deterministic.
    [[nodiscard]] std::vector<uint32_t> observerIndices() const;

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

    // Builds every observer's RWR picture by INVERTING the contact tables (#526). Serial, run once
    // after the parallel sensing pass: for each emitting observer that holds a radar/laser contact on
    // a target, the target (if itself an observer) gains a threat warning naming that emitter. Reading
    // all tables and writing threat sets cannot be data-parallel the way the sensing pass is — an
    // emitter writes into its TARGETS' state, not its own — but it is O(observers × contacts) over
    // already-computed data and trivially serial-equivalent. Must run on the sim thread.
    void buildThreatWarnings(uint64_t tickIndex);

    // Missile-launch RWR seam (#960). A source of threats that are NOT derivable from the observer
    // contact tables — specifically an in-flight RADAR-guided missile guiding on a target (the
    // "launch"/active warning): the missile's active seeker (ARH pitbull) or the illuminator riding
    // it is a legitimate emission an RWR hears. The projectile pool lives outside engine-sensor, so
    // the owner (WorldBroadcaster) injects a provider that enumerates guiding missiles and pushes one
    // ThreatWarning per (missile → target) through the sink. The sink routes it to the target's RWR
    // (only if the target carries one) and it then shares the same cap/ordering as inverted contacts,
    // so a Launch — the highest ThreatLevel — is never crowded out by mere strobes. Null = disabled
    // (unit tests, or a build with no projectiles), leaving the pre-#960 contact-only behavior.
    using ThreatSink = std::function<void(uint32_t targetIdx, const ThreatWarning& warning)>;
    using MissileThreatProvider = std::function<void(const ThreatSink& sink)>;
    void setMissileThreatProvider(MissileThreatProvider provider) {
        m_missileThreatProvider = std::move(provider);
    }

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
    IffResolver m_iffResolver;                     // null = affiliation-rule fallback (see setIffResolver)
    MissileThreatProvider m_missileThreatProvider; // null = contact-only RWR (see setMissileThreatProvider)

    std::unordered_map<uint32_t, ObserverState> m_observers;
    std::vector<ObserverWork> m_work; // reused across ticks; no per-tick allocation
    float m_maxSignatureScale{1.f};

    [[nodiscard]] std::shared_ptr<const SensorDef> resolve(const std::string& id) const;
};

} // namespace fl::sensor
