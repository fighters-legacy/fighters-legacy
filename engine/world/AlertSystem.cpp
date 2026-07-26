// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AlertSystem.h"

#include "flight/Geodetic.h" // geodeticAltitude, kEarthRadiusM -- header-only, no link edge
#include "world/FactionRegistry.h"
#include "world/ZoneGeometry.h"

#include <cstdint>
#include <iterator>

namespace fl {

namespace {

// The stage the cumulative dwell alone justifies. A threshold of 0 is already satisfied at entry,
// which is what makes an all-zero war-state row mean "weapons free the instant you cross".
[[nodiscard]] EscalationStage stageForDwell(const EscalationDwell& d, double dwellS) noexcept {
    if (dwellS >= d.hostileDwellS)
        return EscalationStage::Hostile;
    if (dwellS >= d.interceptDwellS)
        return EscalationStage::Intercept;
    if (dwellS >= d.warningDwellS)
        return EscalationStage::Warned;
    return EscalationStage::InZone;
}

} // namespace

AlertSystem::AlertSystem(FactionRegistry& registry) : m_registry(registry), m_planetRadiusM(kEarthRadiusM) {}

void AlertSystem::addZone(AirspaceZone zone) {
    // A duplicate id would make the id-keyed queries ambiguous; the mission parser already rejects
    // duplicates, so last-one-wins here is only reachable through the programmatic API.
    auto it = m_zoneIndex.find(zone.id);
    if (it != m_zoneIndex.end()) {
        m_zones[it->second] = std::move(zone);
    } else {
        m_zoneIndex.emplace(zone.id, m_zones.size());
        m_zones.push_back(std::move(zone));
    }
    m_resolutionDirty = true;
}

void AlertSystem::addPolicy(EscalationPolicy policy) {
    const std::string id = policy.id;
    m_policies[id] = std::move(policy);
    m_resolutionDirty = true;
}

void AlertSystem::setPlanetRadius(double radiusM) noexcept {
    if (radiusM > 0.0)
        m_planetRadiusM = radiusM;
}

void AlertSystem::rebuildResolution() {
    m_zoneOwner.assign(m_zones.size(), UINT16_MAX);
    m_zonePolicy.assign(m_zones.size(), &m_defaultPolicy);

    for (std::size_t i = 0; i < m_zones.size(); ++i) {
        m_zoneOwner[i] = m_registry.indexOf(m_zones[i].ownerFactionId);
        if (!m_zones[i].policyId.empty()) {
            auto it = m_policies.find(m_zones[i].policyId);
            if (it != m_policies.end())
                m_zonePolicy[i] = &it->second;
        }
    }
    m_resolutionDirty = false;
}

const EscalationPolicy& AlertSystem::policyForZone(std::size_t zoneIdx) const noexcept {
    return *m_zonePolicy[zoneIdx];
}

void AlertSystem::onTick(double simDt, uint64_t tickIndex) {
    if (m_resolutionDirty)
        rebuildResolution();
    if (m_zones.empty() || !m_sampler)
        return;

    m_scratch.clear();
    m_sampler(m_scratch);

    for (const ZoneEntitySample& e : m_scratch) {
        for (std::size_t zi = 0; zi < m_zones.size(); ++zi) {
            const uint16_t owner = m_zoneOwner[zi];
            // An unresolvable owner cannot enforce anything, and a faction never intrudes on its
            // own airspace.
            if (owner == UINT16_MAX || owner == e.factionIndex)
                continue;

            const AirspaceZone& z = m_zones[zi];
            const IntruderKey key = makeKey(e.entityIdx, static_cast<uint32_t>(zi));
            const double altM = geodeticAltitude(e.pos.x, e.pos.y, e.pos.z, m_planetRadiusM);
            const bool inside = zoneContains(z, e.pos.x, e.pos.z, altM);

            auto it = m_intruders.find(key);
            if (it == m_intruders.end()) {
                // Nothing to track for an entity that has never been inside: not allocating a
                // record per (entity x zone) pair is what keeps the pass cheap with many zones.
                if (!inside)
                    continue;
                it = m_intruders.emplace(key, ZoneIntruderState{}).first;
                it->second.gen = e.entityGen;
            } else if (it->second.gen != e.entityGen) {
                // The pool slot was recycled -- this is a different aircraft, not the one that was
                // being tracked. Start it clean rather than inheriting a dead pilot's escalation.
                it->second = ZoneIntruderState{};
                it->second.gen = e.entityGen;
                if (!inside) {
                    m_intruders.erase(it);
                    continue;
                }
            }

            ZoneIntruderState& st = it->second;
            st.lastSeenTick = tickIndex;

            const AlertLevel level = m_registry.alertLevel(owner);
            const EscalationDwell& dwell = policyForZone(zi).forLevel(level);

            if (inside) {
                if (!st.inside) {
                    st.inside = true;
                    st.cooldownS = 0.0;
                }
                st.dwellS += simDt;

                // A faction already at war with the zone owner is not an unknown to be challenged;
                // it is a belligerent, so it is weapons-free on entry regardless of dwell.
                EscalationStage target = m_registry.areHostile(e.factionIndex, owner) ? EscalationStage::Hostile
                                                                                      : stageForDwell(dwell, st.dwellS);

                // Stage never regresses while the intruder is inside. Lowering the owner's alert
                // level mid-dwell relaxes the schedule for the NEXT intruder; it does not revoke a
                // weapons-free call already made on this one.
                if (static_cast<uint8_t>(target) > static_cast<uint8_t>(st.stage)) {
                    // Report every stage crossed, so a listener watching for Warned still hears it
                    // when a war-state zone jumps straight to Hostile.
                    for (auto s = static_cast<uint8_t>(st.stage) + 1; s <= static_cast<uint8_t>(target); ++s) {
                        st.stage = static_cast<EscalationStage>(s);
                        if (onEscalate)
                            onEscalate(e.entityIdx, z.id, st.stage);
                    }
                }
            } else {
                if (st.inside) {
                    st.inside = false;
                    st.cooldownS = 0.0;
                    if (onZoneExit)
                        onZoneExit(e.entityIdx, z.id);
                }
                if (dwell.complianceReset) {
                    st.cooldownS += simDt;
                    if (st.cooldownS >= dwell.complianceCooldownS) {
                        // Turning back worked: the zone forgets the intruder entirely.
                        m_intruders.erase(it);
                        continue;
                    }
                }
                // Without complianceReset the stage sticks and the dwell timer freezes -- the point
                // of a conflict/war posture is that leaving does not undo the violation.
            }
        }
    }

    // Prune records whose entity no longer appears in the sample list (killed or despawned).
    for (auto it = m_intruders.begin(); it != m_intruders.end();)
        it = (it->second.lastSeenTick != tickIndex) ? m_intruders.erase(it) : std::next(it);
}

AlertLevel AlertSystem::getAlertLevel(const std::string& factionId) const {
    return m_registry.alertLevel(m_registry.indexOf(factionId));
}

void AlertSystem::setAlertLevel(const std::string& factionId, AlertLevel level) {
    const uint16_t idx = m_registry.indexOf(factionId);
    if (idx == UINT16_MAX)
        return;
    if (m_registry.alertLevel(idx) == level)
        return; // no-op writes must not produce a wire broadcast
    m_registry.setAlertLevel(idx, level);
    if (onAlertLevelChange)
        onAlertLevelChange(idx, level);
}

EscalationStage AlertSystem::getIntruderStage(uint32_t entityIdx, const std::string& zoneId) const {
    auto zit = m_zoneIndex.find(zoneId);
    if (zit == m_zoneIndex.end())
        return EscalationStage::Clean;
    auto it = m_intruders.find(makeKey(entityIdx, static_cast<uint32_t>(zit->second)));
    return it == m_intruders.end() ? EscalationStage::Clean : it->second.stage;
}

bool AlertSystem::isInZone(uint32_t entityIdx, const std::string& zoneId) const {
    auto zit = m_zoneIndex.find(zoneId);
    if (zit == m_zoneIndex.end())
        return false;
    auto it = m_intruders.find(makeKey(entityIdx, static_cast<uint32_t>(zit->second)));
    return it != m_intruders.end() && it->second.inside;
}

const AirspaceZone* AlertSystem::zone(const std::string& zoneId) const noexcept {
    auto it = m_zoneIndex.find(zoneId);
    return it == m_zoneIndex.end() ? nullptr : &m_zones[it->second];
}

std::vector<std::string> AlertSystem::unresolvedZoneIds() const {
    std::vector<std::string> out;
    // Resolution is only valid after a rebuild; report against the current registry directly so the
    // query is usable before the first tick (which is when the host wants to log it).
    for (const AirspaceZone& z : m_zones)
        if (m_registry.indexOf(z.ownerFactionId) == UINT16_MAX)
            out.push_back(z.id);
    return out;
}

} // namespace fl
