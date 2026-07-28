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

    // Game-master overview map toggle (#861) — opens the battlespace map for GM-capable peers
    GmMap,

    // Voice radio nets (Epic J, #531). Two PTT keys, not one, because the pair of nets a pilot uses
    // constantly is "my flight" and "everyone else on my side" — reaching a menu to switch between
    // them mid-merge is exactly the ceremony the radio-net model exists to avoid. VoiceNetCycle
    // re-points the PRIMARY key at another net for the rarer ATC/proximity calls.
    PushToTalkPrimary,
    PushToTalkSecondary,
    VoiceNetCycle,

    // Voice wingman commands (#935). A THIRD key, not one of the two above: those transmit to other
    // players, this one is captured locally, transcribed on this machine and never leaves it except
    // as a command ordinal. Sharing a key would mean either broadcasting every order to the team or
    // silently swallowing radio calls the pilot meant others to hear.
    WingmanVoiceCommand,

    Count
};

} // namespace fl
