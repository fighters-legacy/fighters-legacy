// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

#include <cstdint>
#include <glm/glm.hpp>

namespace fl::ai {

// Flies a runway approach and landing: track the extended centreline down a glidepath, flare near the
// deck, touch down, and brake to a stop, then go neutral (Done). The air-to-ground half of the ATC
// arrival flow (#701). Like TakeoffController it is a pure function of the observed EntityState with
// no terrain query — the runway elevation is passed in (flat-runway assumption).
//
// Phases advance off AGL and ground speed: Final -> Flare -> Rollout -> Done. The rollout uses the
// wheel brakes and nosewheel steering added in #700, which is why the landing half of ATC could not
// exist before that landed.
class LandingController : public fl::IEntityController {
  public:
    enum class Phase : uint8_t { Final, Flare, Rollout, Done };

    // threshold        — runway threshold (touchdown end) in world space.
    // headingDeg       — runway heading (compass) the aircraft lands along.
    // runwayElevM      — field elevation (m, MSL); the glidepath and flare are measured against it.
    // glideslopeDeg    — approach glidepath angle (default 3.5 deg).
    // approachSpeedMps — target approach ground speed held on final.
    LandingController(glm::dvec3 threshold, float headingDeg, float runwayElevM, float glideslopeDeg = 3.5f,
                      float approachSpeedMps = 75.f);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t tick, double dt,
                            const fl::AiTickContext& ctx = {}) override;

    [[nodiscard]] Phase phase() const noexcept {
        return m_phase;
    }

  private:
    glm::dvec3 m_threshold;
    float m_headingDeg;
    float m_runwayElevM;
    float m_glideslopeRad;
    float m_approachSpeedMps;
    Phase m_phase{Phase::Final};
};

} // namespace fl::ai
