// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace fl {

// The session modes a control can be live in (#1050).
//
// Two actions bound to the same key are only a CONFLICT when the game reads both of them at the
// same time. `Space` is the gun trigger while you are flying and the replay pause key while you are
// watching a recording — those never coexist, so they are not a collision. `V` on the radio PTT and
// `V` on the master-arm switch DO coexist, and that one shipped: keying the radio silently safed the
// guns (#1050). Encoding "when is this live" beside the binding is what lets `InputBindings` tell
// those two cases apart, instead of either flagging every reuse or (as before) checking nothing.
//
// A mode is a property of the SESSION, not of the camera: `Flight` means this peer is flying an
// aircraft, `Spectate` means it is watching a live one with no ownship (observer, or dead and
// awaiting respawn), `Replay` means it is playing a recording, and `Photo` is the paused
// still-framing sub-mode of a replay. The free-fly camera is live in all four, which is exactly why
// its movement keys have to be distinct from the flight controls.
enum class InputContext : uint16_t {
    None = 0,
    Flight = 1u << 0,
    Spectate = 1u << 1,
    Replay = 1u << 2,
    Photo = 1u << 3,
};

constexpr InputContext operator|(InputContext a, InputContext b) noexcept {
    return static_cast<InputContext>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
constexpr InputContext operator&(InputContext a, InputContext b) noexcept {
    return static_cast<InputContext>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
// True when the two context sets overlap — i.e. some session mode reads both actions.
constexpr bool contextsOverlap(InputContext a, InputContext b) noexcept {
    return (a & b) != InputContext::None;
}

// Common context sets, named so the table below reads as intent rather than bit arithmetic.
inline constexpr InputContext kCtxFlight = InputContext::Flight;
inline constexpr InputContext kCtxSpectate = InputContext::Spectate;
inline constexpr InputContext kCtxPlayback = InputContext::Replay | InputContext::Photo;
inline constexpr InputContext kCtxNoOwnship = InputContext::Spectate | InputContext::Replay | InputContext::Photo;
inline constexpr InputContext kCtxLive = InputContext::Flight | InputContext::Spectate;
inline constexpr InputContext kCtxAnyMode =
    InputContext::Flight | InputContext::Spectate | InputContext::Replay | InputContext::Photo;

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
    // Momentary override to full throttle while held (#1050) — this was a raw LeftShift read in
    // FlightInputCollector and had no action at all, so the table said LeftShift was the afterburner
    // while the game used it for the throttle.
    ThrottleMax,
    Airbrake,
    Afterburner,
    // Wheel brakes (#700) — only bite in ground contact.
    WheelBrake,

    // Weapons
    FireWeapon,
    // Release the SELECTED store. Named for what it does: it fires whatever is on the selected
    // station (missile, bomb, rocket pod), which is why the old `FireMissile` name was wrong.
    FireStore,
    NextWeapon,
    PrevWeapon,
    // Master arm toggle (#641) — ARM/SAFE; SAFE gates the fire triggers
    MasterArm,
    // Radar mode cycle (#526/#528): Silent -> Search -> TWS -> STT
    RadarModeCycle,
    // Electronic warfare (#529): dispense chaff + flare, and toggle the ECM jammer
    CountermeasureDispense,
    EcmToggle,

    // Airframe configuration switches (#639)
    LandingGear,
    Flaps,
    ArrestorHook,
    CanopyToggle,

    // Cockpit view pan — a keyboard/d-pad alternative to RMB look, live in Cockpit/Padlock only
    ViewUp,
    ViewDown,
    ViewLeft,
    ViewRight,

    // Free-fly camera (#1050). These were raw SDL scancode reads, which is why the conflict checker
    // could not see that the camera's pan-up key was also the countermeasure dispenser.
    FreeCamForward,
    FreeCamBack,
    FreeCamLeft,
    FreeCamRight,
    FreeCamUp,
    FreeCamDown,
    FreeCamFaster,
    FreeCamSlower,
    FreeCamReset,

    // Shell / overlays
    Pause,
    ConsoleToggle,
    PerfOverlayCycle,

    // Comms — opens the radio menu for ordering your flight (#610)
    WingmanMenu,
    // ATC / ground-crew comms menu (#704)
    CommsMenu,

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

    // Radar MFD (#642) — cycle the display page / the range scale
    MfdPage,
    MfdRange,

    // Night-vision goggles toggle (#210)
    NvgToggle,

    // The in-flight aircraft manual (#821). It is NON-MODAL — the aircraft keeps flying while you
    // read it — so its scroll keys are real bindings with their own defaults, not borrowed page
    // keys: PageUp/PageDown are the throttle, and scrolling the manual used to move it (#1050).
    AircraftManual,
    ManualScrollUp,
    ManualScrollDown,

    // Game-master overview map toggle (#861) — opens the battlespace map for GM-capable peers
    GmMap,

    // Multi-crew seat picker (#975) — cycle joinable seats, join the selected one, leave the current
    CrewSeatCycle,
    CrewSeatJoin,
    CrewSeatLeave,

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

    // Spectator entity picker (#860). Separate actions from NextWeapon/PrevWeapon even though they
    // share a default key: a spectator has no ownship, so the two are never live together, and
    // keeping them apart means rebinding the station selector does not move the spectator picker.
    SpectateNext,
    SpectatePrev,

    // Replay transport (#41). Live only while a recording is playing.
    ReplayPauseToggle,
    ReplaySeekBack,
    ReplaySeekForward,
    ReplaySeekBackFar,
    ReplaySeekForwardFar,
    ReplaySeekStart,
    ReplaySeekEnd,
    ReplaySpeedDown,
    ReplaySpeedUp,

    // Photo mode (#41). Entering it pauses playback, so the speed keys above are dead here and the
    // exposure keys may reuse them.
    PhotoModeToggle,
    PhotoFovIn,
    PhotoFovOut,
    PhotoFovFine,
    PhotoRollLeft,
    PhotoRollRight,
    PhotoEvDown,
    PhotoEvUp,
    PhotoCapture,

    Count
};

} // namespace fl
