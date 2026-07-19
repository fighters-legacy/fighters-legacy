// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

#include <cstdint>
#include <glm/glm.hpp>

namespace fl::ai {

// Flies a runway takeoff: line up on the threshold, accelerate on the runway heading, rotate at Vr,
// and climb out on heading to a target AGL, then go neutral (Done). It is the ground-to-air half of
// the ATC departure flow (#701) and, like every controller, a pure function of the observed
// EntityState — it holds no terrain query, so the runway elevation is passed in (flat-runway
// assumption; correct for a real airfield, which is graded flat by #486's runway flattening).
//
// The phases advance off the aircraft's own ground speed and AGL, so the controller is self-driving:
// LineUp -> Roll -> Rotate -> Climb -> Done. At Done it outputs neutral surfaces and holds throttle,
// leaving the outer StateMachineController (the ATC departure composition, #702) to transition away
// on an external condition such as AboveAltitude.
class TakeoffController : public fl::IEntityController {
  public:
    enum class Phase : uint8_t { LineUp, Roll, Rotate, Climb, Done };

    // threshold      — runway threshold (departure end) in world space.
    // headingDeg     — runway heading (compass, 0 = North, 90 = East) the aircraft rolls along.
    // runwayElevM    — field elevation (m, MSL) — AGL is measured against this flat reference.
    // rotateSpeedMps — Vr: ground speed at which the nose is rotated up.
    // climboutAglM   — AGL at which the climb-out is complete and the controller goes neutral.
    TakeoffController(glm::dvec3 threshold, float headingDeg, float runwayElevM, float rotateSpeedMps = 70.f,
                      float climboutAglM = 300.f);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t tick, double dt,
                            const fl::AiTickContext& ctx = {}) override;

    [[nodiscard]] Phase phase() const noexcept {
        return m_phase;
    }

  private:
    glm::dvec3 m_threshold;
    float m_headingDeg;
    float m_runwayElevM;
    float m_rotateSpeedMps;
    float m_climboutAglM;
    Phase m_phase{Phase::LineUp};
};

} // namespace fl::ai
