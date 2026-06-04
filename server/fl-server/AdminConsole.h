// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <csignal>
#include <functional>
#include <string>
#include <unordered_set>

class DebugCommandRegistry;
class DiscoveryBeacon;
class GameLoop;
class ILogger;

namespace fl {
class EntityManager;
class EntityTypeRegistry;
class WeatherController;
class WorldBroadcaster;
} // namespace fl

// Context injected into server admin commands. All pointers may be nullptr;
// commands check for their required pointers and return an error string if unavailable.
struct ServerCommandContext {
    fl::WorldBroadcaster* broadcaster{nullptr};
    fl::EntityManager* entityManager{nullptr};
    fl::EntityTypeRegistry* typeRegistry{nullptr};
    fl::WeatherController* weatherController{nullptr};
    DiscoveryBeacon* beacon{nullptr}; // for reload_config name update
    GameLoop* gameLoop{nullptr};      // for enqueueSimCallback
    ILogger* logger{nullptr};         // for reload_config parse logging
    std::string* configPath{nullptr}; // path to server.toml, for reload_config
    std::chrono::steady_clock::time_point startTime{};
    volatile sig_atomic_t* quitFlag{nullptr}; // quit command sets this to 1

    // Ban/allowlist file persistence. Null = no file configured.
    std::string* banlistPath{nullptr};
    std::string* allowlistPath{nullptr};
    // Callbacks into main.cpp file I/O; called from sim thread (via enqueueSimCallback).
    std::function<void(const std::unordered_set<std::string>&)> saveBanlist;
    // Callbacks called on the main thread to load IP list files.
    std::function<std::unordered_set<std::string>()> loadBanlist;
    std::function<std::unordered_set<std::string>()> loadAllowlist;
};

// Register all fl-server admin commands into registry using the given context.
void registerServerCommands(DebugCommandRegistry& registry, ServerCommandContext ctx);
