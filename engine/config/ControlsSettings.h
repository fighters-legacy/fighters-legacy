// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Persisted under the [controls] section of user.toml.
// Gamepad axis mapping, per-axis deadzone/curve/invert/scale, and button bindings are
// configured in config/bindings.toml (fl::InputBindings + fl::AxisConfigTable in engine/input/).
struct ControlsSettings {
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
