// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <span>

namespace fl {

class IInput;
class IWindow;

// Menu / game screen lifecycle states.
// Screen::Quit is the "Exit to Desktop" sentinel; update() returns it when the player
// chooses to exit the application. The run loop handles it before any transition.
enum class Screen {
    MainMenu,
    ServerBrowser, // #143: multiplayer server browser (LAN + lobby list)
    JoinServer,    // #322: multiplayer direct-connect form (host / password / callsign)
    Loading,
    MissionSelect,
    MissionBrief,
    Settings,
    Flight,
    Pause,
    Debrief,
    Quit,
};

// A transition into Loading starts a new game session (Game::startGame builds the per-session
// LoadingScreen + FlightScreen). Loading is ONLY ever entered to begin a session, so any prev other
// than Loading itself starts one — this covers both the sandbox path (MainMenu -> Loading) and the
// mission path (MissionSelect -> MissionBrief -> Loading). The old guard fired only on
// prev == MainMenu, so starting a mission from the brief screen left the LoadingScreen null and
// crashed on the next frame (#876). Pure predicate, so the transition rule is unit-testable.
[[nodiscard]] inline bool entersSession(Screen prev, Screen next) noexcept {
    return next == Screen::Loading && prev != Screen::Loading;
}

// A transition back to the main menu from any in-session screen ends the session (Game::stopGame).
[[nodiscard]] inline bool exitsSession(Screen prev, Screen next) noexcept {
    return next == Screen::MainMenu &&
           (prev == Screen::Flight || prev == Screen::Pause || prev == Screen::Debrief || prev == Screen::Loading);
}

// Interface for a single game/menu screen.
// update() is called once per frame to process input and return the next screen.
// buildElements() returns overlay HudElements to submit this frame.
// Returned spans remain valid until the next call to buildElements().
class IScreen {
  public:
    virtual ~IScreen() = default;
    virtual Screen update(IInput& input, IWindow& window) = 0;
    virtual std::span<const HudElement> buildElements() = 0;
};

} // namespace fl
