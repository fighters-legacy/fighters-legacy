// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>

#include "IWindow.h"
#include "IRenderer.h"
#include "IAudio.h"
#include "IInput.h"
#include "INetwork.h"
#include "IFilesystem.h"
#include "ILogger.h"

// Aggregate of all HAL interface instances. Constructed by the platform entry
// point (e.g. platform/sdl3/), populated with concrete backend implementations,
// and passed to the engine on startup. The engine holds a Platform by value;
// all interfaces are exclusively owned here and destroyed with it.
//
// Mix-and-match is valid: a release build might use the SDL3 window/input
// backend and the Vulkan renderer while tests use a null renderer stub.
//
// Recommended initialization order (call ->init() in this sequence):
//   1. logger     — must be ready before any other init() can log failures
//   2. filesystem — asset paths needed by subsequent inits
//   3. window     — required by renderer (nativeHandle for VkSurfaceKHR)
//   4. renderer   — depends on window
//   5. audio      — independent of window/renderer
//   6. input      — shares SDL3 event loop with window (same backend object)
//   7. network    — independent; init last as it may open a port immediately
//
// C++ destroys members in reverse declaration order, so logger is declared
// first here to ensure it outlives every other interface during shutdown.
struct Platform {
    std::unique_ptr<ILogger>     logger;    // first declared → last destroyed
    std::unique_ptr<IFilesystem> filesystem;
    std::unique_ptr<IWindow>     window;
    std::unique_ptr<IRenderer>   renderer;
    std::unique_ptr<IAudio>      audio;
    std::unique_ptr<IInput>      input;
    std::unique_ptr<INetwork>    network;
};
