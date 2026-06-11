// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h" // EnvironmentState
#include "console/CommandRegistry.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

class ILogger;

namespace fl {
class EntityTypeRegistry;
class SimRenderBridge;
} // namespace fl

// Launches fl-server as a subprocess bound to localhost for single-player mode.
// The game client connects to it as a normal ENet client — no server
// infrastructure code lives in the game binary.
//
// LocalServer.h intentionally does NOT include EntityManager.h, WorldBroadcaster.h,
// GameLoop.h, WeatherController.h, or ENetNetwork.h.
class LocalServer {
  public:
    explicit LocalServer(ILogger& log);
    ~LocalServer();

    // Find fl-server binary and spawn it on bindAddr:port.
    // Blocks until fl-server logs "listening on" (up to 3 s) or fails.
    // Returns false on launch failure or readiness timeout.
    bool start(const char* bindAddr = "127.0.0.1", uint16_t port = 4778);

    // Graceful shutdown: send "quit" to admin console, wait 2 s, then kill.
    void stop();

    bool isRunning() const;

    // Initial environment state from the first MsgWeatherState (defaults to
    // PartlyCloudy, 09:00 if no snapshot has arrived yet).
    EnvironmentState initialEnvironment() const;

    // Register server-side console commands (spawn, kill, tp, set_weather).
    // serverCommand is called with formatted command strings and sends them to fl-server
    // via MsgAdminCommand over ENet (constructed by makeNetworkAdminSender in main.cpp).
    void registerConsoleCommands(CommandRegistry& registry, std::function<void(std::string_view)> serverCommand,
                                 fl::SimRenderBridge& renderBridge, fl::EntityTypeRegistry* typeRegistry,
                                 uint32_t* playerEntityIdx, uint32_t* playerEntityGen, bool* showPos);

    // Returns the per-session admin token generated at start(). Valid after start() returns true.
    std::string_view sessionToken() const;

  private:
    ILogger& m_log;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
