// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The client's backend selection, injected at the composition root (#1067).
//
// `game-client` is a library that names no client backend: the renderer and the GUI arrive as
// factories from `main.cpp`, the same way the transport arrives through `createNetwork()`. That is
// what lets the library configure — and its tests run — in the CI legs built without a Vulkan SDK
// (asan, tsan, fuzz, the scale gate), where `platform-vulkan` and `platform-gui` do not exist as
// targets at all.
//
// SDL3 is the one backend the library still names directly (window, input, joystick, display, cursor,
// audio capture): it is always available — cmake/dependencies.cmake FetchContent-builds it when the
// system has no package — and Game reaches through SDL3Window to install the input and joystick sinks,
// which no factory signature would express.
//
// Every factory here is required. A null one is a wiring mistake, not a "backend off" mode: there is
// no second renderer to fall back to, and Game asserts rather than dereferencing an empty function.
// "Running silent" is a decision Game makes AFTER asking for an audio backend (--no-audio, or a
// device that will not open) — not something expressed by withholding the factory.

#include "IAudio.h"
#include "IGui.h"
#include "IRenderer.h"

#include <functional>
#include <memory>

namespace fl {

class IWindow;

struct ClientBackends {
    // The render backend. Called once per session — twice in the process, since the headless path
    // (#913) builds its own and presents to owned images instead of a swapchain.
    std::function<std::unique_ptr<IRenderer>()> createRenderer;

    // The IGui backend, built after the window and renderer exist because it binds to both.
    std::function<std::unique_ptr<IGui>(IWindow&, IRenderer&)> createGui;

    // The audio backend. Game probes it: if init() fails it logs what happened and swaps in NullAudio,
    // so this factory returning a device that cannot open is an expected outcome, not an error.
    std::function<std::unique_ptr<IAudio>()> createAudio;

    [[nodiscard]] bool complete() const {
        return createRenderer && createGui && createAudio;
    }
};

} // namespace fl
