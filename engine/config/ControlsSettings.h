// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Persisted under the [controls] section of user.toml.
struct ControlsSettings {
    float gamepadDeadzone{0.05f}; // clamped to [0, 0.99] on load to prevent div-by-zero
    bool invertPitch{false};
    bool invertRoll{false};
    bool invertRudder{false};
    bool invertThrottle{false};
};
