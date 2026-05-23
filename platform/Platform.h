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
struct Platform {
    std::unique_ptr<IWindow>     window;
    std::unique_ptr<IRenderer>   renderer;
    std::unique_ptr<IAudio>      audio;
    std::unique_ptr<IInput>      input;
    std::unique_ptr<INetwork>    network;
    std::unique_ptr<IFilesystem> filesystem;
    std::unique_ptr<ILogger>     logger;
};
