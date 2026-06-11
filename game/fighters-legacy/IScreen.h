// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <span>

class IInput;
class IWindow;

// Menu / game screen lifecycle states.
// Screen::Quit is the "Exit to Desktop" sentinel; update() returns it when the player
// chooses to exit the application. The run loop handles it before any transition.
enum class Screen {
    MainMenu,
    Loading,
    MissionSelect,
    MissionBrief,
    Settings,
    Flight,
    Pause,
    Debrief,
    Quit,
};

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
