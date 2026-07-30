// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

// The pre-#1061 `[controls]` HOTAS axis mapping, read for MIGRATION ONLY.
//
// Until #1061 the four HOTAS axes lived here instead of in the binding table: read by index on device
// 0, in a config path neither `InputBindings` nor the conflict checker could see — the same defect
// class #1050 exists to remove, surviving in the one corner that fix did not reach. They are now
// `JoystickAxis` bindings plus `[[axis_config]]` entries in bindings.toml.
//
// This struct is still PARSED so a player who retuned those keys does not lose the tuning when
// bindings.toml is regenerated at version 3; `migrateLegacyHotas` folds it into the table. It is never
// WRITTEN back, so once an install has a version-3 bindings.toml the section is inert and can be
// deleted by hand. `present` is false when user.toml named none of the keys, in which case the shipped
// defaults already say the same thing.
struct LegacyHotasAxes {
    bool present{false};
    int aileronAxis{0};
    int elevatorAxis{1};
    int throttleAxis{2};
    int rudderAxis{3};
    float deadzone{0.05f};
    bool invertPitch{false};
    bool invertRoll{false};
    bool invertRudder{false};
    bool invertThrottle{false};
};

// Persisted under the [controls] section of user.toml.
//
// There are no INPUT MAPPINGS left here (#1061): keyboard, mouse, gamepad and raw joystick bindings,
// plus every axis's deadzone / curve / inversion / scale, live in config/bindings.toml via
// fl::InputBindings + fl::AxisConfigTable. What remains is force feedback, which is an OUTPUT.
// Do NOT include IInput.h here — engine/config must not reach into platform/.
struct ControlsSettings {
    // Force feedback (#928): cueing effects on an FFB stick (stall buffet / ground roll / gun kick).
    bool ffbEnabled{true};
    float ffbStrength{1.0f}; // [0, 1]

    // Migration input only — see LegacyHotasAxes.
    LegacyHotasAxes legacyHotas{};
};

} // namespace fl
