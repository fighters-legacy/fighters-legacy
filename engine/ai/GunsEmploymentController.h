// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

namespace fl::ai {

// Guns employment (#462): the controller that makes AI-vs-AI gunfights land hits. Steers the nose
// onto the BALLISTIC LEAD point (computeBallisticLead — muzzle velocity, shooter-velocity carry,
// gravity drop) rather than the target, and holds trigger discipline: it fires only when the
// predicted miss distance at the target's range is inside the lethal radius AND the target is
// inside the gun's reach. Spraying at a target you cannot hit is how movie extras shoot.
//
// Honest targeting like every other controller: the target resolves through TargetView, so the
// lead is computed from the CONTACT's last-known state — an AI cannot lead what it cannot see.
class GunsEmploymentController : public fl::IEntityController {
  public:
    GunsEmploymentController(const fl::EntityManager& entityManager, fl::EntityId targetId, float muzzleVelMps = 1030.f,
                             float lethalRadiusM = 8.f, float maxRangeM = 1200.f, float throttle = 0.9f);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                            const fl::AiTickContext& ctx = {}) override;

    void setTarget(fl::EntityId id) noexcept {
        m_targetId = id;
    }

  private:
    const fl::EntityManager& m_entityManager;
    fl::EntityId m_targetId;
    float m_muzzleVelMps;
    float m_lethalRadiusM;
    float m_maxRangeM;
    float m_throttle;
};

} // namespace fl::ai
