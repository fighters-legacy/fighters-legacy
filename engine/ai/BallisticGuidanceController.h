// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace fl::ai {

// Ballistic missile guidance (#355): boost-phase steering toward a target IMPACT POINT, then
// inertial flight — the controller keeps emitting commands after burnout, but a ballistic vehicle's
// only control authority is thrust vectoring (BallisticForceModel), so a burned-out vehicle ignores
// them exactly like the real article. Reentry is pure ballistics.
//
// The pitch program: hold the loft pitch (default 45° — the max-range trajectory), and STEEPEN when
// the continuously-predicted vacuum impact point overshoots the target — a steeper trajectory above
// 45° trades range for altitude, so overshoot bleeds off while an undershooting shot simply flies
// max-range and lands honest-short. Heading is held on the target's local-tangent bearing. All
// deterministic: no dice anywhere.
//
// MIRV: once past apogee, a bus configured with `mirvCount > 0` raises SpawnRequests (the #355
// controller-spawn seam) deploying child RVs on deterministically fanned velocities; each child
// flies this same controller (with no MIRV of its own) and inherits the parent's ownership chain,
// so an RV kill credits whoever launched the bus.
class BallisticGuidanceController : public fl::IEntityController {
  public:
    struct Params {
        glm::dvec3 targetPos{};   // world-space impact point
        float basePitchDeg{45.f}; // the loft program's floor (max range)
        int mirvCount{0};         // 0 = unitary
        double mirvSpreadM{2000.0};
        std::string mirvTypeId; // child entity type; EMPTY = same type as the bus
    };

    explicit BallisticGuidanceController(Params p) : m_p(std::move(p)) {}

    fl::ControlInput sample(const fl::EntityState& state, uint64_t tick, double dt,
                            const fl::AiTickContext& ctx = {}) override;

    std::vector<fl::SpawnRequest> drainSpawnRequests() override {
        std::vector<fl::SpawnRequest> out;
        out.swap(m_pending);
        return out;
    }

    [[nodiscard]] bool mirvDeployed() const noexcept {
        return m_mirvDeployed;
    }

  private:
    Params m_p;
    bool m_mirvDeployed{false};
    std::vector<fl::SpawnRequest> m_pending;
};

} // namespace fl::ai
