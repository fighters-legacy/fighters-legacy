// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

namespace fl::ai {

// Lead pursuit / proportional navigation controller.
// Steers toward a predicted intercept point computed from the target's current velocity and
// estimated time-to-intercept. More realistic than pure pursuit: avoids tail-chasing and
// converges on a collision course.
//
// navGain scales the lead: 0.0 degenerates to pure pursuit, 1.0 = first-order TTC lead
// (standard), higher values exaggerate lead for very fast targets.
//
// Returns neutral ControlInput when the target is dead or invalid.
class LeadPursuitController : public fl::IEntityController {
  public:
    LeadPursuitController(const fl::EntityManager& entityManager, fl::EntityId targetId, float navGain = 1.0f,
                          float throttle = 0.9f, bool useAfterburner = false);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

    void setTarget(fl::EntityId id) noexcept {
        m_targetId = id;
    }

  private:
    const fl::EntityManager& m_entityManager;
    fl::EntityId m_targetId;
    float m_navGain;
    float m_throttle;
    bool m_useAfterburner;
};

} // namespace fl::ai
