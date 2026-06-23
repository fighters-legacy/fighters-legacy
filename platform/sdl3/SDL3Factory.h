// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Thin factory header. Include this instead of SDL3Window.h / SDL3Input.h /
// SDL3Display.h so consumers are never exposed to SDL3 headers.
#include "IDisplay.h"
#include "IInput.h"
#include "IWindow.h"
#include <memory>

namespace fl {

// Returns a wired window + input pair. The input backend is already registered
// as an event sink with the window; calling window->pollEvents() dispatches
// keyboard, mouse, and scroll events into the input state.
//
// Member order is load-bearing: input is declared first so it is destroyed
// LAST — after window — ensuring the window never holds a dangling sink pointer
// during its own destruction (C++ destroys members in reverse declaration order).
struct SDL3WindowInput {
    std::unique_ptr<IInput> input;   // destroyed last — outlives window
    std::unique_ptr<IWindow> window; // destroyed first
};
SDL3WindowInput createSDL3WindowInput();

std::unique_ptr<IDisplay> createSDL3Display();

} // namespace fl
