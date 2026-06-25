// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

namespace fl::ai {

// Low yo-yo: dive-and-cut-corner maneuver to close on a turning target.
//   Phase 1 (Dive): bank toward target, unload (negative elevator) and accelerate to descend
//                   into the target's turn plane and cut the corner.
//   Phase 2 (Pull): pull up to re-engage at target altitude while continuing to steer.
//   Phase 3 (Done): neutral controls until a StateMachineController transitions away.
// Returns neutral ControlInput when the target is dead or invalid.
class LowYoYoController : public fl::IEntityController {
  public:
    LowYoYoController(const fl::EntityManager& entityManager, fl::EntityId targetId, float diveDurationS = 1.5f,
                      float pullDurationS = 2.5f);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

    void setTarget(fl::EntityId id) noexcept {
        m_targetId = id;
    }

  private:
    enum class Phase { Dive, Pull, Done };

    const fl::EntityManager& m_entityManager;
    fl::EntityId m_targetId;
    float m_diveDuration;
    float m_pullDuration;

    Phase m_phase{Phase::Dive};
    float m_timer{0.f};
};

} // namespace fl::ai
