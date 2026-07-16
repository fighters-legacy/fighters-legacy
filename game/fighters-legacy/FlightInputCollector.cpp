// SPDX-License-Identifier: GPL-3.0-or-later
#include "FlightInputCollector.h"

#include "CameraInput.h"
#include "IInput.h"
#include "IJoystick.h"
#include "config/ControlsSettings.h"
#include "console/GameConsole.h"
#include "render/SimRenderBridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace fl {

std::optional<fl::MsgClientInput> FlightInputCollector::poll(const fl::SimRenderBridge& /*bridge*/,
                                                             CameraInput& camInput, const GameConsole& console,
                                                             IInput& input, IJoystick* joystick,
                                                             const ControlsSettings& cs, bool uiFocused) {
    m_weaponFired = false;

    const auto now = m_clock->now();
    if (std::chrono::duration<float>(now - m_lastInputTime).count() < 1.0f / 60.0f)
        return std::nullopt;
    m_lastInputTime = now;

    fl::MsgClientInput inp;
    inp.seqNum = m_inputSeq++;
    // tickIndex + ackMask (the snapshot ack) are stamped by ClientNetEventHandler::stampAck() at the
    // send site (#566) — the net handler is the single ack authority. Left default (0) here.

    constexpr float kThrottleStep = 1.0f / 60.0f;
    if (!console.isOpen()) {
        if (input.isKeyDown(Key::PageUp))
            camInput.adjustThrottle(kThrottleStep);
        if (input.isKeyDown(Key::PageDown))
            camInput.adjustThrottle(-kThrottleStep);
        inp.throttle = input.isKeyDown(Key::LeftShift) ? 1.f : camInput.throttle();
        inp.elevator = (input.isKeyDown(Key::ArrowUp) ? -1.f : 0.f) + (input.isKeyDown(Key::ArrowDown) ? 1.f : 0.f);
        inp.aileron = (input.isKeyDown(Key::ArrowRight) ? 1.f : 0.f) + (input.isKeyDown(Key::ArrowLeft) ? -1.f : 0.f);
        inp.rudder = (input.isKeyDown(Key::X) ? 1.f : 0.f) + (input.isKeyDown(Key::Z) ? -1.f : 0.f);
        inp.buttons = input.isKeyDown(Key::Space) ? 1u : 0u;
        if (input.isKeyDown(Key::Tab))
            inp.buttons |= 0x02u;
        // Fire the selected store (#625): mouse-right or the Enter key. Level on the wire — the
        // server's FireControl edge-detects, so holding it is one shot. Gated while an overlay owns
        // the discrete keys (the radio menu's picks are the digit keys the cycle uses, #610).
        if (!uiFocused && (input.isMouseButtonDown(MouseButton::Right) || input.isKeyDown(Key::Enter)))
            inp.buttons |= 0x04u;
        // Weapon-station cycling (#625): local edge detection, ABSOLUTE selection on the wire (an
        // absolute value converges on a lossy channel; a lost cycle-edge would not). Wraps within
        // the station count Game.cpp provides from the entity def; 0 = unknown (selection off).
        const bool nextDown = !uiFocused && input.isKeyDown(Key::Num1); // NextWeapon primary binding
        const bool prevDown = !uiFocused && input.isKeyDown(Key::Num2); // PrevWeapon primary binding
        if (m_stationCount > 0) {
            const bool nextEdge = nextDown && !m_prevNextKey;
            const bool prevEdge = prevDown && !m_prevPrevKey;
            // 255 = "keep the server's default selection". Only a deliberate cycle replaces it —
            // an untouched selector must never override what the server chose at spawn.
            if ((nextEdge || prevEdge) && m_selectedStation == 255)
                m_selectedStation = 0;
            else if (nextEdge)
                m_selectedStation = static_cast<uint8_t>((m_selectedStation + 1) % m_stationCount);
            else if (prevEdge)
                m_selectedStation = static_cast<uint8_t>((m_selectedStation + m_stationCount - 1) % m_stationCount);
        }
        m_prevNextKey = nextDown;
        m_prevPrevKey = prevDown;
        m_weaponFired = (inp.buttons & 1u) != 0u;

        if (input.getGamepadCount() > 0) {
            auto readAxis = [&](fl::InputAction action) -> float {
                const fl::Binding b = m_bindings.get(action, /*alt=*/true);
                if (b.source != fl::BindingSource::GamepadAxis)
                    return 0.0f;
                const auto ax = static_cast<GamepadAxis>(b.id);
                return m_axisConfig.get(ax).apply(input.getGamepadAxis(0, ax));
            };

            // ThrottleAxis: TriggerLeft returns [0,1] unipolar. apply() handles deadzone + scale.
            // Note: AxisConfig::invert for a unipolar trigger negates to [-1,0]; std::clamp brings
            // it back to 0 (throttle off). Inverted throttle is better configured via the HOTAS path.
            const float thr = readAxis(fl::InputAction::ThrottleAxis);
            if (thr != 0.0f) {
                camInput.setThrottle(std::clamp(thr, 0.0f, 1.0f));
                inp.throttle = camInput.throttle();
            }
            const float elev = readAxis(fl::InputAction::PitchAxis);
            if (elev != 0.0f)
                inp.elevator = elev;
            const float ail = readAxis(fl::InputAction::RollAxis);
            if (ail != 0.0f)
                inp.aileron = ail;
            const float rud = readAxis(fl::InputAction::YawAxis);
            if (rud != 0.0f)
                inp.rudder = rud;

            auto readButton = [&](fl::InputAction action) -> bool {
                const fl::Binding b = m_bindings.get(action, /*alt=*/true);
                if (b.source == fl::BindingSource::GamepadButton)
                    return input.isGamepadButtonDown(0, static_cast<GamepadButton>(b.id));
                if (b.source == fl::BindingSource::GamepadAxis) {
                    const float v = input.getGamepadAxis(0, static_cast<GamepadAxis>(b.id));
                    return b.axisNegative ? (v < -0.5f) : (v > 0.5f);
                }
                return false;
            };

            // While an overlay owns the discrete inputs, its confirm button must not also pull the
            // trigger. The axes above are deliberately still live.
            if (!uiFocused) {
                if (readButton(fl::InputAction::FireWeapon)) {
                    inp.buttons |= 1u;
                    m_weaponFired = true;
                }
                if (readButton(fl::InputAction::Afterburner))
                    inp.buttons |= 0x02u;
                if (readButton(fl::InputAction::FireMissile))
                    inp.buttons |= 0x04u; // fire selected store (#625)
                // Station cycling on the gamepad (D-pad by default), same edge/wrap logic as keys.
                const bool padNext = readButton(fl::InputAction::NextWeapon);
                const bool padPrev = readButton(fl::InputAction::PrevWeapon);
                if (m_stationCount > 0) {
                    const bool nextEdge = padNext && !m_prevPadNext;
                    const bool prevEdge = padPrev && !m_prevPadPrev;
                    if ((nextEdge || prevEdge) && m_selectedStation == 255)
                        m_selectedStation = 0;
                    else if (nextEdge)
                        m_selectedStation = static_cast<uint8_t>((m_selectedStation + 1) % m_stationCount);
                    else if (prevEdge)
                        m_selectedStation =
                            static_cast<uint8_t>((m_selectedStation + m_stationCount - 1) % m_stationCount);
                }
                m_prevPadNext = padNext;
                m_prevPadPrev = padPrev;
            }
        }

        // HOTAS / raw joystick blend — throttle always sets absolute position;
        // stick/pedal axes win when |axis| > hotasDeadzone.
        if (joystick && joystick->getJoystickCount() > 0) {
            const int axCount = joystick->getAxisCount(0);
            const float hdz = cs.hotasDeadzone;
            auto applyHotas = [hdz](float raw) -> float {
                const float mag = std::abs(raw);
                if (mag <= hdz)
                    return 0.0f;
                return std::copysign((mag - hdz) / (1.0f - hdz), raw);
            };
            // Full-range [-1, 1] → [0, 1]; absolute position device.
            if (cs.hotasThrottleAxis >= 0 && cs.hotasThrottleAxis < axCount) {
                float raw = joystick->getAxisValue(0, cs.hotasThrottleAxis);
                if (cs.hotasInvertThrottle)
                    raw = -raw;
                camInput.setThrottle(std::clamp((raw + 1.0f) * 0.5f, 0.0f, 1.0f));
                inp.throttle = camInput.throttle();
            }
            if (cs.hotasElevatorAxis >= 0 && cs.hotasElevatorAxis < axCount) {
                const float elev = applyHotas(joystick->getAxisValue(0, cs.hotasElevatorAxis));
                if (elev != 0.0f)
                    inp.elevator = cs.hotasInvertPitch ? -elev : elev;
            }
            if (cs.hotasAileronAxis >= 0 && cs.hotasAileronAxis < axCount) {
                const float ail = applyHotas(joystick->getAxisValue(0, cs.hotasAileronAxis));
                if (ail != 0.0f)
                    inp.aileron = cs.hotasInvertRoll ? -ail : ail;
            }
            if (cs.hotasRudderAxis >= 0 && cs.hotasRudderAxis < axCount) {
                const float rud = applyHotas(joystick->getAxisValue(0, cs.hotasRudderAxis));
                if (rud != 0.0f)
                    inp.rudder = cs.hotasInvertRudder ? -rud : rud;
            }
        }
    } else {
        inp.throttle = camInput.throttle();
    }

    // Absolute station selection rides every packet (#625): idempotent under loss, converges.
    inp.selectedStation = m_selectedStation;

    // Camera eye world-position (#858): where this client is looking FROM. The server keys interest
    // management on it for an entity-less peer (an observer ghost camera, or a dead peer), and
    // ignores it for a pilot (the aircraft transform wins). Sent every frame regardless of mode.
    const glm::dvec3 eye = camInput.eyeWorld();
    inp.cameraEye[0] = eye.x;
    inp.cameraEye[1] = eye.y;
    inp.cameraEye[2] = eye.z;
    return inp;
}

void FlightInputCollector::setClock(const fl::IClock& clock) {
    m_clock = &clock;
}

void FlightInputCollector::setBindings(fl::InputBindings bindings) {
    m_bindings = std::move(bindings);
}

void FlightInputCollector::setAxisConfig(fl::AxisConfigTable cfg) {
    m_axisConfig = std::move(cfg);
}

} // namespace fl