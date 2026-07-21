// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace fl {

enum class InputAction : uint32_t {
    // Continuous axes
    PitchAxis,
    RollAxis,
    YawAxis,
    ThrottleAxis,

    // Digital flight controls
    PitchUp,
    PitchDown,
    RollLeft,
    RollRight,
    YawLeft,
    YawRight,
    ThrottleUp,
    ThrottleDown,
    Airbrake,
    Afterburner,

    // Weapons
    FireWeapon,
    FireMissile,
    NextWeapon,
    PrevWeapon,

    // View
    ViewUp,
    ViewDown,
    ViewLeft,
    ViewRight,

    // Systems
    LandingGear,
    Flaps,
    Pause,
    Menu,

    // Comms — opens the radio menu for ordering your flight (#610)
    WingmanMenu,

    // Emergency egress — command the ejection seat (#672)
    Eject,

    // Respawn after death in a multiplayer match (#648)
    Respawn,

    Count
};

} // namespace fl
