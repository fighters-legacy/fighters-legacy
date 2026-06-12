// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// Persisted under the [controls] section of user.toml.
struct ControlsSettings {
    float gamepadDeadzone{0.05f}; // clamped to [0, 0.99] on load to prevent div-by-zero
    bool invertPitch{false};
    bool invertRoll{false};
    bool invertRudder{false};
    bool invertThrottle{false};

    // Gamepad primary-fire button. Mapped to GamepadButton enum values (IInput.h).
    // Valid range [0, 15]; clamped on load. Default 5 = RightShoulder.
    uint8_t fireButton{5};

    // Gamepad afterburner button. Mapped to GamepadButton enum values (IInput.h).
    // Valid range [0, 15]; clamped on load. Default 4 = LeftShoulder.
    uint8_t afterburnerButton{4};

    // HOTAS / raw joystick axis assignments. Index into IJoystick::getAxisValue(0, n).
    // -1 disables the mapping; clamped to [-1, 127] on load.
    int hotasAileronAxis{0};
    int hotasElevatorAxis{1};
    int hotasThrottleAxis{2};
    int hotasRudderAxis{3};
    float hotasDeadzone{0.05f}; // clamped to [0, 0.99] on load
    bool hotasInvertPitch{false};
    bool hotasInvertRoll{false};
    bool hotasInvertRudder{false};
    bool hotasInvertThrottle{false};
};
