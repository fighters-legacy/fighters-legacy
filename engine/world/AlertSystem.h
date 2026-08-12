// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "loop/ISimUpdate.h"
#include "world/AirspaceZone.h"
#include "world/AlertLevel.h"
#include "world/EscalationPolicy.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

class FactionRegistry;

// The minimal per-entity view the zone pass needs (#162). A POD so engine-world needs no
// engine-entity dependency: the host fills a scratch vector each tick through the sampler seam,
// the same std::function injection setGroundElevationQuery / setAtcTransmissionSink use.
struct ZoneEntitySample {
    uint32_t entityIdx{0};
    uint16_t entityGen{0};    // pool generation; a recycled index is a NEW intruder, not the old one
    uint16_t factionIndex{0}; // 0 = neutral/unaffiliated -- still challenged, it just has no side
    glm::dvec3 pos{0.0};      // world metres, Y up
};

// Escalation record for one (intruder, zone) pair. Dwell is cumulative from entry, matching the
// EscalationDwell thresholds; cooldown runs only while the intruder is outside and the policy arms
// compliance reset.
struct ZoneIntruderState {
    EscalationStage stage{EscalationStage::Clean};
    double dwellS{0};
    double cooldownS{0};
    bool inside{false};
    uint16_t gen{0};
    // Tick this record was last touched. A record whose entity stops appearing in the sample list
    // has left the sim (killed, despawned), so the pass prunes it -- otherwise a long match
    // accumulates a record per entity that ever entered a zone.
    uint64_t lastSeenTick{0};
};

// Server-authoritative airspace enforcement (#162). Zones come from the mission's `airspace_zones:`
// section, escalation policies from content-pack TOML, and the zone owner's live AlertLevel (held by
// FactionRegistry) selects which dwell row applies -- so raising a faction's alert level tightens
// every zone it owns at once.
//
// Registered with GameLoop like every other sim system (#1078). It implemented ISimUpdate from the
// start, with a comment explaining why it was NOT registered with the loop it implemented the interface
// for -- WorldBroadcaster was the engine's single ISimUpdate, so fl-server drove second consumers from
// an end-of-tick hook instead. The loop drives an ordered list now, so the interface means what it says.
// Order matters here and is data: this must run AFTER the world has stepped, so a zone test sees where
// everyone actually is this tick rather than where they were last tick.
//
// Threading: onTick() and every query run on the sim thread. setAlertLevel() delegates to
// FactionRegistry, whose alert-level storage is mutex-guarded for the network/main thread -- but the
// onAlertLevelChange callback then fires on the CALLING thread, so a host that calls it off the sim
// thread must route the broadcast accordingly (fl-server goes through enqueueSimCallback).
class AlertSystem : public ISimUpdate {
  public:
    explicit AlertSystem(FactionRegistry& registry);

    // ── configuration (before the first tick) ─────────────────────────────────
    void addZone(AirspaceZone zone);
    void addPolicy(EscalationPolicy policy);

    // Fills `out` with every live entity to test this tick. Called once per tick; the vector is a
    // reused scratch buffer, so the host must clear() before filling. Unset = no intruders.
    using EntitySampler = std::function<void(std::vector<ZoneEntitySample>& out)>;
    void setEntitySampler(EntitySampler sampler) {
        m_sampler = std::move(sampler);
    }

    // Planet radius used to turn a world position into an altitude above the datum for the zone's
    // altitude band. Defaults to Earth; fl-server wires [world] planet_radius_m.
    void setPlanetRadius(double radiusM) noexcept;

    // ── the per-tick pass ─────────────────────────────────────────────────────
    void onTick(double simDt, uint64_t tickIndex) override;

    // ── alert levels (delegated to FactionRegistry, which owns the storage) ────
    [[nodiscard]] AlertLevel getAlertLevel(const std::string& factionId) const;
    void setAlertLevel(const std::string& factionId, AlertLevel level);

    // ── queries (Lua bindings, tests) ─────────────────────────────────────────
    [[nodiscard]] EscalationStage getIntruderStage(uint32_t entityIdx, const std::string& zoneId) const;
    [[nodiscard]] bool isInZone(uint32_t entityIdx, const std::string& zoneId) const;
    [[nodiscard]] std::size_t zoneCount() const noexcept {
        return m_zones.size();
    }
    [[nodiscard]] const AirspaceZone* zone(const std::string& zoneId) const noexcept;

    // Zones whose `owner:` does not resolve against the faction registry. Such a zone enforces
    // nothing, which is invisible in play -- the host logs these once at startup rather than letting
    // a mission quietly ship with an airspace that never challenges anyone.
    [[nodiscard]] std::vector<std::string> unresolvedZoneIds() const;

    // ── event callbacks (wired by the host; always null-checked before calling) ─
    // An intruder advanced to `stage` in `zoneId`. Fires once per stage crossing, in ascending stage
    // order, so a jump from Clean to Hostile reports every stage it passed through -- a script that
    // only listens for Warned still hears it in a war-state zone.
    std::function<void(uint32_t entityIdx, const std::string& zoneId, EscalationStage stage)> onEscalate;
    // An intruder left the zone. Fires on the inside->outside edge, not at the end of cooldown.
    std::function<void(uint32_t entityIdx, const std::string& zoneId)> onZoneExit;
    // A faction's alert level changed (setAlertLevel with a different value). Fires on the caller's
    // thread; see the threading note above.
    std::function<void(uint16_t factionIndex, AlertLevel level)> onAlertLevelChange;

  private:
    // (entityIdx << 32) | zoneIndex -- one record per intruder/zone pair.
    using IntruderKey = uint64_t;
    [[nodiscard]] static constexpr IntruderKey makeKey(uint32_t entityIdx, uint32_t zoneIdx) noexcept {
        return (static_cast<uint64_t>(entityIdx) << 32) | zoneIdx;
    }

    void rebuildResolution();
    [[nodiscard]] const EscalationPolicy& policyForZone(std::size_t zoneIdx) const noexcept;

    FactionRegistry& m_registry;

    std::vector<AirspaceZone> m_zones;
    std::unordered_map<std::string, std::size_t> m_zoneIndex;
    std::unordered_map<std::string, EscalationPolicy> m_policies;
    EscalationPolicy m_defaultPolicy{defaultEscalationPolicy()};

    // Resolved per zone, rebuilt whenever a zone or policy is added (m_resolutionDirty).
    std::vector<uint16_t> m_zoneOwner;                 // UINT16_MAX = unresolved
    std::vector<const EscalationPolicy*> m_zonePolicy; // never null after rebuild
    bool m_resolutionDirty{true};

    std::unordered_map<IntruderKey, ZoneIntruderState> m_intruders;

    EntitySampler m_sampler;
    std::vector<ZoneEntitySample> m_scratch;
    double m_planetRadiusM;
};

} // namespace fl
