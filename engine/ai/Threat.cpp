// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/Threat.h"

#include "ai/Guidance.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "spatial/SpatialIndex.h"
#include "world/FactionRegistry.h" // hostile() — coalition-aware, nullptr = affiliation fallback (#632)

#include <cmath>

namespace fl::ai {
namespace {

// Candidate ranking: smallest angular offset from boresight, ties broken by range. Angle dominates
// because "my target" is what the lead is LOOKING at — a closer bandit off to the side is not it.
struct Candidate {
    fl::EntityId id{};
    float cosAngle{-2.f}; // larger = closer to boresight
    double rangeSq{0.0};

    [[nodiscard]] bool betterThan(const Candidate& other) const noexcept {
        if (cosAngle != other.cosAngle) {
            return cosAngle > other.cosAngle;
        }
        return rangeSq < other.rangeSq;
    }
};

// Shared per-candidate test for both queries. Returns false when `other` is not a live hostile.
bool isLiveHostile(const fl::EntityState& observer, uint16_t observerFaction, const fl::EntityState* other,
                   const fl::FactionRegistry* factions) noexcept {
    if (!other || other->dead || other->id.index == observer.id.index) {
        return false;
    }
    return fl::hostile(factions, observerFaction, other->factionIndex);
}

} // namespace

fl::EntityId designateBoresightTarget(const fl::EntityManager& em, const fl::EntityState& lead, const float viewAxis[3],
                                      float maxRangeM, float halfAngleRad, const fl::SpatialIndex* si,
                                      const fl::FactionRegistry* factions) {
    if (lead.factionIndex == 0 || maxRangeM <= 0.f) {
        return fl::EntityId{}; // a neutral entity has no enemies; nothing to designate
    }

    // A zero/degenerate view axis (a client that never sent one) falls back to where the nose points.
    glm::vec3 axis(viewAxis[0], viewAxis[1], viewAxis[2]);
    const float axisLen = glm::length(axis);
    axis = (axisLen > 1e-4f) ? axis / axisLen : bodyForward(lead.transform.quat);

    const float minCos = std::cos(halfAngleRad);
    const double maxRangeSq = static_cast<double>(maxRangeM) * static_cast<double>(maxRangeM);
    const glm::dvec3 leadPos(lead.transform.pos[0], lead.transform.pos[1], lead.transform.pos[2]);

    Candidate best{};

    const auto consider = [&](const fl::EntityState& other) {
        const glm::dvec3 otherPos(other.transform.pos[0], other.transform.pos[1], other.transform.pos[2]);
        const glm::dvec3 delta = otherPos - leadPos;
        const double rangeSq = glm::dot(delta, delta);
        if (rangeSq > maxRangeSq || rangeSq < 1e-6) {
            return; // out of range, or coincident (no meaningful bearing)
        }
        const glm::vec3 dir = glm::normalize(glm::vec3(delta));
        const float cosAngle = glm::dot(dir, axis);
        if (cosAngle < minCos) {
            return; // outside the boresight cone
        }
        Candidate cand{other.id, cosAngle, rangeSq};
        if (cand.betterThan(best)) {
            best = cand;
        }
    };

    if (si) {
        // queryRadius is cell-level conservative — the exact range test lives in `consider`.
        si->queryRadius(lead.transform.pos, static_cast<double>(maxRangeM), [&](uint32_t idx, const double* /*pos*/) {
            const fl::EntityState* other = em.getByIndex(idx);
            if (isLiveHostile(lead, lead.factionIndex, other, factions)) {
                consider(*other);
            }
        });
    } else {
        em.forEach([&](const fl::EntityState& other) {
            if (isLiveHostile(lead, lead.factionIndex, &other, factions)) {
                consider(other);
            }
        });
    }

    return best.id; // default-constructed (invalid) when nothing qualified
}

fl::EntityId designateFromContacts(const fl::EntityState& lead, const float viewAxis[3],
                                   const fl::sensor::ContactTable* contacts, float maxRangeM, float halfAngleRad,
                                   const fl::FactionRegistry* factions) {
    if (!contacts || contacts->empty())
        return {}; // no sensing, or nothing seen — refuse rather than invent a target

    const glm::vec3 axis(viewAxis[0], viewAxis[1], viewAxis[2]);
    const float axisLen = glm::length(axis);
    if (axisLen < 1e-4f)
        return {};
    const glm::vec3 look = axis / axisLen;

    const glm::dvec3 leadPos(lead.transform.pos[0], lead.transform.pos[1], lead.transform.pos[2]);
    const float cosLimit = std::cos(halfAngleRad);
    const double maxRangeSq = static_cast<double>(maxRangeM) * static_cast<double>(maxRangeM);

    fl::EntityId bestLocked{};
    float bestLockedDot = -2.f;
    fl::EntityId bestSeen{};
    float bestSeenDot = -2.f;

    for (const fl::sensor::Contact& c : *contacts) {
        if (!fl::hostile(factions, lead.factionIndex, c.factionIndex))
            continue;

        // LAST-KNOWN position: what the lead actually knows, not where the target really is.
        const glm::dvec3 tgt(c.lastKnownPos[0], c.lastKnownPos[1], c.lastKnownPos[2]);
        const glm::dvec3 d = tgt - leadPos;
        const double distSq = glm::dot(d, d);
        if (distSq > maxRangeSq || distSq < 1e-6)
            continue;

        const glm::vec3 dir = glm::normalize(glm::vec3(d));
        const float dot = glm::dot(dir, look);
        if (dot < cosLimit)
            continue; // outside the boresight cone

        // A lock beats a bearing. Among equals, the one nearest the nose wins.
        if (c.locked()) {
            if (dot > bestLockedDot) {
                bestLockedDot = dot;
                bestLocked = c.id;
            }
        } else if (dot > bestSeenDot) {
            bestSeenDot = dot;
            bestSeen = c.id;
        }
    }

    return bestLocked.valid() ? bestLocked : bestSeen;
}

fl::EntityId nearestHostileWithin(const fl::EntityManager& em, const fl::EntityState& anchor, uint16_t selfFaction,
                                  float rangeM, const fl::SpatialIndex* si, const fl::FactionRegistry* factions) {
    if (selfFaction == 0 || rangeM <= 0.f) {
        return fl::EntityId{};
    }

    const double maxRangeSq = static_cast<double>(rangeM) * static_cast<double>(rangeM);
    const glm::dvec3 anchorPos(anchor.transform.pos[0], anchor.transform.pos[1], anchor.transform.pos[2]);

    fl::EntityId best{};
    double bestRangeSq = maxRangeSq;

    const auto consider = [&](const fl::EntityState& other) {
        const glm::dvec3 otherPos(other.transform.pos[0], other.transform.pos[1], other.transform.pos[2]);
        const glm::dvec3 delta = otherPos - anchorPos;
        const double rangeSq = glm::dot(delta, delta);
        if (rangeSq <= bestRangeSq) {
            bestRangeSq = rangeSq;
            best = other.id;
        }
    };

    if (si) {
        si->queryRadius(anchor.transform.pos, static_cast<double>(rangeM), [&](uint32_t idx, const double* /*pos*/) {
            const fl::EntityState* other = em.getByIndex(idx);
            if (isLiveHostile(anchor, selfFaction, other, factions)) {
                consider(*other);
            }
        });
    } else {
        em.forEach([&](const fl::EntityState& other) {
            if (isLiveHostile(anchor, selfFaction, &other, factions)) {
                consider(other);
            }
        });
    }

    return best;
}

} // namespace fl::ai
