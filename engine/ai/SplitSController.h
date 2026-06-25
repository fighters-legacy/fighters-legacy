// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

namespace fl::ai {

// Split-S maneuver: roll inverted then pull through the bottom of a loop to reverse heading.
// Opposite energy trade to the Immelmann — loses altitude, gains airspeed.
//   Phase 1 (Roll): aileron roll to inverted with idle throttle + speedbrake for rollDurationS seconds.
//   Phase 2 (Pull): full elevator pull with full throttle for pullDurationS seconds, exiting right-side up.
//   Phase 3 (Done): neutral controls until a StateMachineController transitions away.
class SplitSController : public fl::IEntityController {
  public:
    explicit SplitSController(float rollDurationS = 1.5f, float pullDurationS = 4.0f);

    fl::ControlInput sample(const fl::EntityState& /*state*/, uint64_t /*tick*/, double dt,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

  private:
    enum class Phase { Roll, Pull, Done };

    float m_rollDuration;
    float m_pullDuration;

    Phase m_phase{Phase::Roll};
    float m_timer{0.f};
};

} // namespace fl::ai
