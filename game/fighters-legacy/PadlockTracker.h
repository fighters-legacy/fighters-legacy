// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "spatial/LineOfSight.h" // LosResult

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fl {

// Padlock lock state (#697). Off when not padlocked; Breaking is the grace window after visibility is
// lost; Reacquire returns the aim toward the airframe forward while it waits for the target to reappear.
enum class PadlockState : uint8_t { Off, Locked, Breaking, Reacquire };

struct PadlockInputs {
    float dt{1.0f / 60.0f};
    glm::dvec3 ownPos{0.0}; // cockpit eye position (world)
    glm::quat ownOrient{1, 0, 0, 0};
    glm::dvec3 targetPos{0.0};              // velocity-extrapolated target position (world), supplied by the caller
    LosResult terrainLos{LosResult::Clear}; // latched terrain LOS; Unknown is treated as Clear here
    glm::vec3 worldUp{0, 1, 0};             // local radial up at the eye
};

struct PadlockPose {
    glm::vec3 forward{0, 0, -1};
    glm::vec3 up{0, 1, 0};
    PadlockState state{PadlockState::Off};
    bool exitToCockpit{false}; // set on the frame the reacquire window expires — the caller reverts to Cockpit
};

// The pure padlock aim + lock state machine (#697). Holds ALL the testable math: a continuous world-
// space aim direction slewed toward the (extrapolated) target with a damped, rate-limited approach; an
// elevation clamp (no pitch wrap across the vertical — overhead crossings resolve smoothly, never a 2π
// jump); a cockpit visibility envelope; and a Locked -> Breaking (0.4 s grace) -> Reacquire (4 s window)
// -> Off state machine. Frame-rate independent by construction (everything scales by dt).
class PadlockTracker {
  public:
    // Seed the aim from the current view so entering padlock never pops.
    void enter(glm::vec3 currentForward, glm::vec3 currentUp);
    void exit() noexcept {
        m_state = PadlockState::Off;
    }

    PadlockPose update(const PadlockInputs& in);

    [[nodiscard]] PadlockState state() const noexcept {
        return m_state;
    }

  private:
    glm::vec3 m_aim{0, 0, -1}; // continuous world-space aim direction
    PadlockState m_state{PadlockState::Off};
    float m_breakTimer{0.0f};
    float m_reacquireTimer{0.0f};
};

} // namespace fl
