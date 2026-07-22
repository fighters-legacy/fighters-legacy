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

    // Hold to show the multiplayer scoreboard overlay (#647)
    Scoreboard,

    // Open the in-match chat input box — all channel / team channel (#646)
    ChatAll,
    ChatTeam,

    // Camera modes (#689) — rebindable, gamepad-capable replacements for the old raw F1/F2/F4 reads.
    CameraCockpit,
    CameraChase,
    CameraFree,

    // Target designation + padlock/inset view family (#671/#689)
    PadlockToggle,
    NextTarget,
    PrevTarget,
    TargetInsetToggle,

    // Player autopilot holds (#640) — altitude / heading / speed hold toggles
    AutopilotAltHold,
    AutopilotHdgHold,
    AutopilotSpdHold,

    // Master arm toggle (#641) — ARM/SAFE; SAFE gates the fire triggers
    MasterArm,

    // Radar MFD (#642) — cycle the display page / the range scale
    MfdPage,
    MfdRange,

    // Night-vision goggles toggle (#210)
    NvgToggle,

    Count
};

} // namespace fl
