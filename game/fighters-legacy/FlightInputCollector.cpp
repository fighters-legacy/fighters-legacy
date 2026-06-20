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

std::optional<fl::MsgClientInput> FlightInputCollector::poll(const fl::SimRenderBridge& bridge, CameraInput& camInput,
                                                             const GameConsole& console, IInput& input,
                                                             IJoystick* joystick, const ControlsSettings& cs) {
    m_weaponFired = false;

    const auto now = m_clock->now();
    if (std::chrono::duration<float>(now - m_lastInputTime).count() < 1.0f / 60.0f)
        return std::nullopt;
    m_lastInputTime = now;

    fl::MsgClientInput inp;
    inp.seqNum = m_inputSeq++;
    inp.tickIndex = bridge.hasSnapshot() ? bridge.current().tickIndex : 0;

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
        m_weaponFired = (inp.buttons & 1u) != 0u;

        // Gamepad axis blend — axis mapping from m_bindings alt slots, processing from m_axisConfig.
        // Only GamepadAxis-type alt bindings are used here; GamepadButton actions are handled separately.
        if (input.getGamepadCount() > 0) {
            // Look up which GamepadAxis is bound to 'action' (alt slot), apply per-axis config.
            // Returns 0.0f when the binding is absent or not a GamepadAxis.
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

            // Gamepad button actions: still read from ControlsSettings; migrate to InputBindings in #312.
            if (input.isGamepadButtonDown(0, static_cast<GamepadButton>(cs.fireButton))) {
                inp.buttons |= 1u;
                m_weaponFired = true;
            }
            if (input.isGamepadButtonDown(0, static_cast<GamepadButton>(cs.afterburnerButton)))
                inp.buttons |= 0x02u;
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
