// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/RenderSnapshot.h"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <vector>

namespace fl {

// The single client-side source of truth for "the designated target" (#696). Padlock (#697), the
// target-slaved inset (#698), and #641's combat symbology all consume it. Operates purely on
// EntityRenderEntry / RenderSnapshot, so replay playback (#41) inherits it by construction.
//
// The candidate list is produced by a PLUGGABLE provider (the default scans the snapshot). This is the
// #526 upgrade seam: sensor tracks replace the snapshot scan later, and the {idx, gen} handle plus
// every consumer stay unchanged.

struct TargetCandidate {
    uint32_t idx{0};
    uint32_t gen{0};
    double rangeM{0.0};
    bool hostile{false};      // ident == foe (via DesignationContext::identOf)
    float coneAngleRad{0.0f}; // angle between ownship forward and the bearing to this candidate
};

// Everything a candidate provider needs, assembled by FlightScreen each frame. The categoryOf / identOf
// seams keep TargetDesignation free of EntityTypeRegistry / net-handler dependencies.
struct DesignationContext {
    const RenderSnapshot* snap{nullptr};
    uint32_t ownIdx{0};
    uint32_t ownGen{0};
    glm::dvec3 ownPos{0.0};
    glm::vec3 ownForward{0.0f, 0.0f, 1.0f};
    std::function<uint8_t(uint32_t typeIndex)> categoryOf{};    // ObjectCategory ordinal
    std::function<uint8_t(const EntityRenderEntry&)> identOf{}; // kIff* ordinal (#688)
};

class TargetDesignation {
  public:
    using CandidateProvider = std::function<void(const DesignationContext&, std::vector<TargetCandidate>&)>;

    // Swap the candidate source (the #526 sensor-track upgrade point). Null restores the default scan.
    void setProvider(CandidateProvider p) {
        m_provider = std::move(p);
    }

    // Resolve the designated target in the current snapshot. Returns nullptr and auto-clears the
    // designation on a miss (despawn / generation mismatch / destroyed).
    [[nodiscard]] const EntityRenderEntry* resolve(const RenderSnapshot& snap);

    // Cycle to the next (dir > 0) / previous (dir < 0) candidate, in the provider's order. Designates
    // it; a no-candidate cycle clears the designation.
    void cycle(int dir, const DesignationContext& ctx);

    // Designate the best candidate inside a forward cone (smallest cone angle within coneHalfAngleRad),
    // falling back to the nearest candidate when none is in the cone. For padlock's toggle-on with no
    // existing designation. Returns true if something was designated.
    bool designateBest(const DesignationContext& ctx, float coneHalfAngleRad);

    void clear() noexcept {
        m_designated = false;
    }
    [[nodiscard]] bool designated() const noexcept {
        return m_designated;
    }
    [[nodiscard]] uint32_t idx() const noexcept {
        return m_idx;
    }
    [[nodiscard]] uint32_t gen() const noexcept {
        return m_gen;
    }

  private:
    void buildCandidates(const DesignationContext& ctx);

    uint32_t m_idx{0};
    uint32_t m_gen{0};
    bool m_designated{false};
    CandidateProvider m_provider;
    std::vector<TargetCandidate> m_scratch; // reused across frames, no per-frame allocation churn
};

// The default candidate provider (also usable directly): scans the snapshot for targetable entities
// (any category except Projectile / Effect), excludes the ownship and destroyed entities, and orders
// hostile-first then ascending range.
void defaultTargetProvider(const DesignationContext& ctx, std::vector<TargetCandidate>& out);

} // namespace fl
