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

    // Where the embedded server writes its `.flrep` recordings (#41/#643). The server otherwise
    // resolves [replay] dir against its own working directory, which is wherever the CLIENT was
    // launched from -- so single-player recordings would land somewhere the replay browser does not
    // look. The client owns its user-data location, so the client names the directory.
    void setReplayDir(std::string dir);

    enum class StartResult {
        Ok,          // fl-server started and is listening
        SpawnFailed, // subprocess could not be created (binary not found)
        BindFailed,  // fl-server logged "bind failed" or "network init failed"
        Timeout,     // did not log "listening on" within 3 seconds
    };

    // The port the CLIENT-PROVISIONED single-player server binds. Not the 4778 standard game port,
    // for one straightforward reason: a dedicated fl-server on the same machine may already hold it,
    // and enet6 sets no SO_REUSEADDR, so whoever binds second fails outright.
    //
    // It no longer has to dodge the LAN-discovery listener as well. Discovery had aliased the game
    // port — the beacon broadcast to 4778 and the browser bound 4778 — so this constant once had to
    // avoid a whole neighbourhood of derived ports and came with fifteen lines explaining which. With
    // discovery on its own port (fl::kDiscoveryPort, #1071) the only constraint left is the one
    // above, plus the query port fl-server derives as GAME PORT + 1: 4776 derives 4777, and neither
    // is claimed by anything. test_local_server_port locks that.
    //
    // start() still passes --no-discovery: a server with one loopback player should not advertise
    // itself on the LAN or answer queries. That is a correctness choice about visibility now rather
    // than a port-collision workaround.
    static constexpr uint16_t kLocalServerPort = 4776;

    // Find fl-server binary and spawn it on bindAddr:port.
    // Blocks until fl-server logs "listening on" (up to 3 s) or fails.
    // Returns Ok on success; SpawnFailed, BindFailed, or Timeout on failure.
    StartResult start(const char* bindAddr = "127.0.0.1", uint16_t port = kLocalServerPort);

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
    std::string m_replayDir;   // forwarded via --replay-dir; empty = the server's own [replay] dir
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
