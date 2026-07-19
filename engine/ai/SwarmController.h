// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

#include <glm/glm.hpp>

namespace fl::ai {

// Tunables for the boids swarm (#353). The three classic terms — separation, alignment, cohesion —
// plus a migration goal, weighted and summed into one desired direction the member then flies with
// the shared bank-to-turn steering.
struct SwarmParams {
    float neighborRadiusM{600.f};   // flockmates inside this range participate
    float separationRadiusM{120.f}; // inside this range, the separation term pushes apart
    float separationWeight{1.5f};   // collision avoidance dominates by default
    float alignmentWeight{1.0f};    // match flock velocity
    float cohesionWeight{0.8f};     // steer toward the local flock centre
    float migrationWeight{1.2f};    // steer toward the goal (point or anchor entity)
    float cruiseThrottle{0.75f};    // baseline power
    float speedGain{0.004f};        // throttle per m/s of flock-speed mismatch (loose speed matching)
    float lookaheadM{400.f};        // steering aim-point projection distance
    uint32_t maxNeighbors{16};      // per-tick neighbor cap: a 200-drone cloud costs each member 16, not 199
};

// Coordinated drone-swarm behavior (#353): separation / alignment / cohesion boids over the shared
// SpatialIndex (AiTickContext::si — the reason the index exists for AI at all; the conservative
// XZ cell query is exact-filtered in 3D here), plus a migration goal. FLOCKMATES ARE THE SAME TYPE
// AND FACTION as the member — a swarm flocks with its own, not with whatever happens to be nearby —
// and the neighbor cap bounds the per-member cost so a big cloud stays O(members × cap), never
// O(N²). Two goal modes: a fixed world point, or a moving anchor entity (a strike lead the swarm
// escorts); an invalid/absent goal leaves a pure flock. Null ctx.si degrades to an EntityManager
// scan (correct, just not cheap), so the controller works in tests and headless harnesses.
class SwarmController : public fl::IEntityController {
  public:
    // Migrate toward a fixed world point.
    SwarmController(const fl::EntityManager& entityManager, const glm::dvec3& migrationPoint, SwarmParams params = {});
    // Follow a moving anchor entity; an invalid id = no migration goal (pure flock).
    SwarmController(const fl::EntityManager& entityManager, fl::EntityId anchor, SwarmParams params = {});

    fl::ControlInput sample(const fl::EntityState& state, uint64_t tick, double dt,
                            const fl::AiTickContext& ctx = {}) override;

  private:
    const fl::EntityManager& m_entityManager;
    glm::dvec3 m_migrationPoint{};
    bool m_hasPoint{false};
    fl::EntityId m_anchor{};
    SwarmParams m_params;
};

} // namespace fl::ai
