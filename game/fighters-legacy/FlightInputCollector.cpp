// SPDX-License-Identifier: GPL-3.0-or-later
#include "FlightInputCollector.h"

#include "CameraInput.h"
#include "IInput.h"
#include "IJoystick.h"
#include "config/ControlsSettings.h"
#include "console/GameConsole.h"
#include "render/SimRenderBridge.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>

std::optional<fl::MsgClientInput> FlightInputCollector::poll(const fl::SimRenderBridge& bridge, CameraInput& camInput,
                                                             const GameConsole& console, IInput& input,
                                                             IJoystick* joystick, const ControlsSettings& cs) {
    m_weaponFired = false;

    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    if (std::chrono::duration<float>(now - m_lastInputTime).count() < 1.0f / 60.0f)
        return std::nullopt;
    m_lastInputTime = now;

    const bool* keys = SDL_GetKeyboardState(nullptr);
    fl::MsgClientInput inp;
    inp.seqNum = m_inputSeq++;
    inp.tickIndex = bridge.hasSnapshot() ? bridge.current().tickIndex : 0;

    constexpr float kThrottleStep = 1.0f / 60.0f;
    if (!console.isOpen()) {
        if (keys[SDL_SCANCODE_PAGEUP])
            camInput.adjustThrottle(kThrottleStep);
        if (keys[SDL_SCANCODE_PAGEDOWN])
            camInput.adjustThrottle(-kThrottleStep);
        inp.throttle = keys[SDL_SCANCODE_LSHIFT] ? 1.f : camInput.throttle();
        inp.elevator = (keys[SDL_SCANCODE_UP] ? -1.f : 0.f) + (keys[SDL_SCANCODE_DOWN] ? 1.f : 0.f);
        inp.aileron = (keys[SDL_SCANCODE_RIGHT] ? 1.f : 0.f) + (keys[SDL_SCANCODE_LEFT] ? -1.f : 0.f);
        inp.rudder = (keys[SDL_SCANCODE_X] ? 1.f : 0.f) + (keys[SDL_SCANCODE_Z] ? -1.f : 0.f);
        inp.buttons = keys[SDL_SCANCODE_SPACE] ? 1u : 0u;
        if (keys[SDL_SCANCODE_TAB])
            inp.buttons |= 0x02u;
        m_weaponFired = (inp.buttons & 1u) != 0u;

        // Gamepad axis blend — wins when |axis| > deadzone.
        if (input.getGamepadCount() > 0) {
            const float dz = cs.gamepadDeadzone;
            auto applyAxis = [dz](float raw) -> float {
                const float mag = std::abs(raw);
                if (mag <= dz)
                    return 0.0f;
                return std::copysign((mag - dz) / (1.0f - dz), raw);
            };
            // TriggerLeft [0,1] → absolute throttle when above deadzone.
            const float trig = input.getGamepadAxis(0, GamepadAxis::TriggerLeft);
            if (trig > dz) {
                const float t = (trig - dz) / (1.0f - dz);
                camInput.setThrottle(cs.invertThrottle ? 1.0f - t : t);
                inp.throttle = camInput.throttle();
            }
            const float elev = applyAxis(input.getGamepadAxis(0, GamepadAxis::RightY));
            if (elev != 0.0f)
                inp.elevator = cs.invertPitch ? -elev : elev;
            const float ail = applyAxis(input.getGamepadAxis(0, GamepadAxis::RightX));
            if (ail != 0.0f)
                inp.aileron = cs.invertRoll ? -ail : ail;
            const float rud = applyAxis(input.getGamepadAxis(0, GamepadAxis::LeftX));
            if (rud != 0.0f)
                inp.rudder = cs.invertRudder ? -rud : rud;
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
