// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

namespace fl::ai {

// Immelmann turn: half-loop + roll to reverse heading with an altitude gain.
//   Phase 1 (Pull): full elevator + afterburner for pullDurationS seconds, climbing to inverted.
//   Phase 2 (Roll): right aileron roll to upright for rollDurationS seconds.
//   Phase 3 (Done): neutral controls until a StateMachineController transitions away.
class ImmelmannController : public fl::IEntityController {
  public:
    explicit ImmelmannController(float pullDurationS = 4.0f, float rollDurationS = 1.5f);

    fl::ControlInput sample(const fl::EntityState& /*state*/, uint64_t /*tick*/, double dt,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

  private:
    enum class Phase { Pull, Roll, Done };

    float m_pullDuration;
    float m_rollDuration;

    Phase m_phase{Phase::Pull};
    float m_timer{0.f};
};

} // namespace fl::ai
