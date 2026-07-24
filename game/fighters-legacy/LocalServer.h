// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h" // EnvironmentState
#include "console/CommandRegistry.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace fl {

class ILogger;
class EntityTypeRegistry;
class SimRenderBridge;

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

    // Content root to forward to the spawned fl-server via --assets, so the embedded server resolves
    // mods/ from the same directory the client did (#831). Empty (default) lets fl-server fall back
    // to its own resolution (the current working directory). Call before start().
    void setContentRoot(std::string root);

    // Mission to load in the embedded server, forwarded via --mission (#634). Empty (default) starts
    // the sandbox — the same behavior as before this seam existed. Call before start().
    void setMission(std::string missionName);

    enum class StartResult {
        Ok,          // fl-server started and is listening
        SpawnFailed, // subprocess could not be created (binary not found)
        BindFailed,  // fl-server logged "bind failed" or "network init failed"
        Timeout,     // did not log "listening on" within 3 seconds
    };

    // Find fl-server binary and spawn it on bindAddr:port.
    // Blocks until fl-server logs "listening on" (up to 3 s) or fails.
    // Returns Ok on success; SpawnFailed, BindFailed, or Timeout on failure.
    StartResult start(const char* bindAddr = "127.0.0.1", uint16_t port = 4778);

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
                                 SimRenderBridge& renderBridge, EntityTypeRegistry* typeRegistry,
                                 uint32_t* playerEntityIdx, uint32_t* playerEntityGen, bool* showPos, bool* showPing,
                                 std::function<std::string()> reloadContent = {},
                                 std::function<void(uint32_t, uint8_t, float)> setArtChannel = {},
                                 std::function<void()> clearArtChannels = {});

    // Returns the per-session admin token generated at start(). Valid after start() returns true.
    std::string_view sessionToken() const;

  private:
    ILogger& m_log;
    std::string m_contentRoot; // forwarded to fl-server via --assets; empty = server resolves its own
    std::string m_mission;     // forwarded to fl-server via --mission; empty = sandbox (#634)
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
