// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <net/AuthTracker.h>

#include <chrono>
#include <csignal>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace fl {

class CommandRegistry;
class CommandShell;
class DiscoveryBeacon;
class GameLoop;
class ILogger;

class EntityManager;
class EntityTypeRegistry;
class WeatherController;
class WorldBroadcaster;

// Context injected into server admin commands, grouped by concern. All pointers may be nullptr;
// commands check for their required pointers and return an error string if unavailable.
struct ServerCommandContext {
    // Live simulation objects. Mutations run on the sim thread via sim.gameLoop->enqueueSimCallback.
    struct SimRefs {
        WorldBroadcaster* broadcaster{nullptr};
        EntityManager* entityManager{nullptr};
        EntityTypeRegistry* typeRegistry{nullptr};
        WeatherController* weatherController{nullptr};
        GameLoop* gameLoop{nullptr}; // for enqueueSimCallback
    } sim;

    // Process/runtime environment.
    struct ServerEnv {
        ILogger* logger{nullptr};         // for reload_config parse logging
        std::string* configPath{nullptr}; // path to server.toml, for reload_config
        std::chrono::steady_clock::time_point startTime{};
        volatile sig_atomic_t* quitFlag{nullptr}; // quit command sets this to 1
        DiscoveryBeacon* beacon{nullptr};         // for reload_config name update
        // Loads an AI script by asset name. Returns {source_bytes_as_string, pack_root_dir}.
        // Empty source = not found. Null = Lua AI scripting unavailable.
        // Must be safe to call from any thread (pre-loaded read-only cache in fl-server).
        std::function<std::pair<std::string, std::string>(std::string_view name)> loadAIScript;
    } env;

    // Shutdown command policy (from ServerConfig [shutdown] section).
    struct ShutdownPolicy {
        uint32_t warningIntervalS{300}; // default 5 min between countdown notices
        uint32_t minDelayS{0};          // 0 = no minimum enforced
        bool requireConfirm{true};      // require --force flag to schedule/trigger shutdown
    } shutdown;

    // Ban/allowlist file persistence. Null paths = no file configured.
    struct BanPersistence {
        std::string* banlistPath{nullptr};
        std::string* allowlistPath{nullptr};
        // saveBanlist is called from the sim thread (via enqueueSimCallback);
        // loadBanlist/loadAllowlist are called on the main thread.
        std::function<void(const std::unordered_set<std::string>&)> saveBanlist;
        std::function<std::unordered_set<std::string>()> loadBanlist;
        std::function<std::unordered_set<std::string>()> loadAllowlist;
    } bans;

    // RCON channel hooks. All null when RCON is not configured.
    struct RconHooks {
        // Clears the RCON auth lockout for an IP (true if a lockout was active);
        // called from the sim thread via enqueueSimCallback.
        std::function<bool(const std::string&)> clearRconLockout;
        // Returns RCON channel auth lockout state.
        std::function<AuthLockoutSummary()> getRconAuthSummary;
        // Optional output shell; sim-callback confirmations are also routed here for
        // RCON drain (issue #304). nullptr = disabled.
        CommandShell* shell{nullptr};
    } rcon;
};

// Register all fl-server admin commands into registry using the given context.
void registerServerCommands(CommandRegistry& registry, ServerCommandContext ctx);

} // namespace fl
