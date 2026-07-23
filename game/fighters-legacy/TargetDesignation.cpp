// SPDX-License-Identifier: GPL-3.0-or-later
#include "TargetDesignation.h"

#include "entity/DamageDef.h"      // DamageLevel::Destroyed
#include "entity/ObjectCategory.h" // ObjectCategory
#include "render/RadarView.h"      // kIffFoe

#include <algorithm>
#include <cmath>

namespace fl {

namespace {
// A category is targetable if it is not a projectile or a cosmetic effect (aircraft, ground/naval
// vehicles, players, structures all qualify).
bool targetableCategory(uint8_t cat) {
    return cat != static_cast<uint8_t>(ObjectCategory::Projectile) &&
           cat != static_cast<uint8_t>(ObjectCategory::Effect);
}
} // namespace

void defaultTargetProvider(const DesignationContext& ctx, std::vector<TargetCandidate>& out) {
    out.clear();
    if (!ctx.snap)
        return;
    const glm::vec3 fwd =
        (glm::dot(ctx.ownForward, ctx.ownForward) > 1e-8f) ? glm::normalize(ctx.ownForward) : glm::vec3{0, 0, 1};

    for (const EntityRenderEntry& e : ctx.snap->entries) {
        if (e.entityIdx == ctx.ownIdx && e.entityGen == ctx.ownGen)
            continue; // never the ownship
        if (e.damageLevel >= static_cast<uint8_t>(DamageLevel::Destroyed))
            continue;
        const uint8_t cat =
            ctx.categoryOf ? ctx.categoryOf(e.typeIndex) : static_cast<uint8_t>(ObjectCategory::AirVehicle);
        if (!targetableCategory(cat))
            continue;

        const glm::dvec3 d = e.position - ctx.ownPos;
        const double rng = glm::length(d);
        float cone = 3.14159265f; // behind if degenerate
        if (rng > 1e-6) {
            const glm::vec3 dir = glm::vec3(d / rng);
            cone = std::acos(std::clamp(glm::dot(fwd, dir), -1.0f, 1.0f));
        }
        const bool hostile = ctx.identOf && ctx.identOf(e) == kIffFoe;
        out.push_back(TargetCandidate{e.entityIdx, e.entityGen, rng, hostile, cone});
    }

    // Hostile-first, then ascending range; stable idx tiebreak so cycling order is deterministic.
    std::sort(out.begin(), out.end(), [](const TargetCandidate& a, const TargetCandidate& b) {
        if (a.hostile != b.hostile)
            return a.hostile; // hostiles first
        if (a.rangeM != b.rangeM)
            return a.rangeM < b.rangeM;
        return a.idx < b.idx;
    });
}

void TargetDesignation::buildCandidates(const DesignationContext& ctx) {
    if (m_provider)
        m_provider(ctx, m_scratch);
    else
        defaultTargetProvider(ctx, m_scratch);
}

const EntityRenderEntry* TargetDesignation::resolve(const RenderSnapshot& snap) {
    if (!m_designated)
        return nullptr;
    for (const EntityRenderEntry& e : snap.entries) {
        if (e.entityIdx == m_idx && e.entityGen == m_gen) {
            if (e.damageLevel >= static_cast<uint8_t>(DamageLevel::Destroyed)) {
                m_designated = false; // it died — drop the designation
                return nullptr;
            }
            return &e;
        }
    }
    // Despawned or the pool slot was reused with a new generation: the handle no longer resolves.
    m_designated = false;
    return nullptr;
}

void TargetDesignation::cycle(int dir, const DesignationContext& ctx) {
    buildCandidates(ctx);
    if (m_scratch.empty()) {
        m_designated = false;
        return;
    }
    // Find the current designation in the list; start from there, else from the first (dir>0) / last.
    int cur = -1;
    if (m_designated) {
        for (std::size_t i = 0; i < m_scratch.size(); ++i)
            if (m_scratch[i].idx == m_idx && m_scratch[i].gen == m_gen) {
                cur = static_cast<int>(i);
                break;
            }
    }
    const int n = static_cast<int>(m_scratch.size());
    const int step = (dir >= 0) ? 1 : -1;
    int next = (cur < 0) ? (step > 0 ? 0 : n - 1) : ((cur + step) % n + n) % n;
    m_idx = m_scratch[next].idx;
    m_gen = m_scratch[next].gen;
    m_designated = true;
}

bool TargetDesignation::designateBest(const DesignationContext& ctx, float coneHalfAngleRad) {
    buildCandidates(ctx);
    if (m_scratch.empty()) {
        m_designated = false;
        return false;
    }
    // Prefer the candidate with the smallest cone angle inside the cone; else the nearest overall.
    const TargetCandidate* best = nullptr;
    for (const TargetCandidate& c : m_scratch) {
        if (c.coneAngleRad <= coneHalfAngleRad) {
            if (!best || c.coneAngleRad < best->coneAngleRad)
                best = &c;
        }
    }
    if (!best) {
        for (const TargetCandidate& c : m_scratch)
            if (!best || c.rangeM < best->rangeM)
                best = &c;
    }
    m_idx = best->idx;
    m_gen = best->gen;
    m_designated = true;
    return true;
}

} // namespace fl
