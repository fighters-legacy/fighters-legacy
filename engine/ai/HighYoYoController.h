// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

namespace fl::ai {

// High yo-yo: energy-management overshoot correction for fast pursuit aircraft.
//   Phase 1 (Climb): bank away from target (half-authority reverse aileron) and pull hard up,
//                    bleeding airspeed and gaining altitude.
//   Phase 2 (Reacquire): roll back toward target and ease in with a moderate elevator pull.
//   Phase 3 (Done): neutral controls until a StateMachineController transitions away.
// Returns neutral ControlInput when the target is dead or invalid.
class HighYoYoController : public fl::IEntityController {
  public:
    HighYoYoController(const fl::EntityManager& entityManager, fl::EntityId targetId, float climbDurationS = 2.5f,
                       float reacquireDurationS = 3.0f);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

    void setTarget(fl::EntityId id) noexcept {
        m_targetId = id;
    }

  private:
    enum class Phase { Climb, Reacquire, Done };

    const fl::EntityManager& m_entityManager;
    fl::EntityId m_targetId;
    float m_climbDuration;
    float m_reacquireDuration;

    Phase m_phase{Phase::Climb};
    float m_timer{0.f};
};

} // namespace fl::ai
