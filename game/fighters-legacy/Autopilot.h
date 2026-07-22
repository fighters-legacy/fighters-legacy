// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/Geodetic.h" // kEarthRadiusM (default planet radius)

#include <cstdint>

namespace fl {

struct FlightState;

// The command an engaged autopilot writes over the player's input this tick. Per-field validity flags
// so a single-axis hold (e.g. HdgHold alone) never clobbers the axes it does not own.
struct AutopilotCommand {
    float elevator{0.0f};
    float aileron{0.0f};
    float rudder{0.0f};
    float throttle{0.0f};
    bool hasPitch{false};    // AltHold engaged -> elevator valid
    bool hasRoll{false};     // HdgHold (or the Alt-only wing leveler) -> aileron + rudder valid
    bool hasThrottle{false}; // SpdHold engaged -> throttle valid
};

// Client-side player autopilot (#640): altitude / heading / speed hold implemented as input shaping
// over the existing control pipeline, so the server stays authoritative and dumb. Pure — it consumes
// the client's predicted FlightState (the only place the client has honest ownship state) and the
// planet radius, and emits control overrides. Toggling a mode captures the current value as its target;
// player stick/throttle input disengages the relevant holds.
//
// The control laws reuse the engine's header-only P-controller primitives (engine/ai/Guidance.h +
// engine/flight/LocalFrame.h), the same math the AI autopilots fly.
class Autopilot {
  public:
    enum ModeBit : uint8_t { AltHold = 1, HdgHold = 2, SpdHold = 4 };

    // Toggle a hold on/off. Engaging captures the current altitude / heading / speed from `s` as the
    // hold target (planetRadiusM needed for the local-level altitude/heading).
    void toggleAltHold(const FlightState& s, double planetRadiusM);
    void toggleHdgHold(const FlightState& s, double planetRadiusM);
    void toggleSpdHold(const FlightState& s);
    void disengageAll() noexcept {
        m_modes = 0;
    }

    // Note the raw (pre-shaping) player command this tick. Elevator/aileron past kStickOverride
    // disengages AltHold + HdgHold; a throttle touch disengages SpdHold only; rudder never disengages.
    void notePlayerInput(float elevator, float aileron, float rudder, bool throttleTouched) noexcept;

    // Compute the control override for this tick. dt is the sim step (1/60 s).
    [[nodiscard]] AutopilotCommand compute(const FlightState& s, float dt, double planetRadiusM) const;

    [[nodiscard]] uint8_t modes() const noexcept {
        return m_modes;
    }
    [[nodiscard]] float targetAltM() const noexcept {
        return m_targetAltM;
    }
    [[nodiscard]] float targetHeadingRad() const noexcept {
        return m_targetHeadingRad;
    }
    [[nodiscard]] float targetSpeedMps() const noexcept {
        return m_targetSpeedMps;
    }

  private:
    uint8_t m_modes{0};
    float m_targetAltM{0.0f};
    float m_targetHeadingRad{0.0f};
    float m_targetSpeedMps{0.0f};
};

} // namespace fl
