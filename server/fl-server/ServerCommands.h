// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ServerUptime.h"
#include "server_config.h"

#include <config/DifficultySettings.h> // AiScaling — server-side difficulty (#682)
#include <net/AdminChannel.h>          // the enumerable admin-frontend registry (#1079)
#include <script/LuaSandbox.h>         // ScriptPackSource — a script's pack root + its filesystem (#1210)

#include <csignal>
#include <functional>
#include <memory>
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
struct WorldApi;
namespace atc {
class AtcService; // engine/atc/AtcService.h — atc_status/atc_scramble/atc_hold (#705)
} // namespace atc

// Context injected into server admin commands, grouped by concern. All pointers may be nullptr;
// commands check for their required pointers and return an error string if unavailable.
struct ServerCommandContext {
    // Live simulation objects. Mutations run on the sim thread via sim.gameLoop->enqueueSimCallback.
    struct SimRefs {
        WorldBroadcaster* broadcaster{nullptr};
        EntityManager* entityManager{nullptr};
        EntityTypeRegistry* typeRegistry{nullptr};
        WeatherController* weatherController{nullptr};
        GameLoop* gameLoop{nullptr};       // for enqueueSimCallback
        const WorldApi* worldApi{nullptr}; // world.* host seam for spawned Lua controllers (#413)
        fl::atc::AtcService* atc{nullptr}; // ATC service for atc_status/atc_scramble/atc_hold (#705)
    } sim;

    // Process/runtime environment.
    struct ServerEnv {
        ILogger* logger{nullptr};         // for reload_config parse logging
        std::string* configPath{nullptr}; // path to server.toml, for reload_config
        // The config the process STARTED with (#1081). reload_config diffs the freshly parsed file
        // against this to name the restart-only keys it ignored, and hot appliers read restart-only
        // values from it so a reload cannot apply one through the back door. Never rewritten: a
        // restart-only key stays "not in effect" until the restart, which is the operator's question.
        const ServerConfig* runningConfig{nullptr};
        // How long the server has been up, for `status`. A ServerUptime and not a bare time_point so
        // that it cannot be the clock epoch, and so that every frontend reports the same number --
        // see ServerUptime.h (#1048). Default-constructed = "started when this context was built",
        // which is a small overstatement of nothing rather than the machine's boot time.
        ServerUptime uptime{};
        volatile sig_atomic_t* quitFlag{nullptr}; // quit command sets this to 1
        DiscoveryBeacon* beacon{nullptr};         // for reload_config name update
        std::string traceDir; // configured [trace] input_trace_dir; trace_start default when no arg (#560)
        // Loads an AI script by asset name. Returns {source_bytes_as_string, where its require()
        // reads modules from}. Empty source = not found. Null = Lua AI scripting unavailable.
        // The pack root travels WITH the filesystem that resolves it (#1210) — it is an
        // Assets-domain path, and resolving it any other way lands on the process working directory.
        // Must be safe to call from any thread (pre-loaded read-only cache in fl-server).
        std::function<std::pair<std::string, fl::ScriptPackSource>(std::string_view name)> loadAIScript;

        // Resolves an [ai] difficulty preset name ("cadet"|"pilot"|"ace") to its AiScaling (#682).
        // Injected by fl-server, which owns the DifficultyMultipliers table (mod-overridable
        // data/difficulty.toml); null = difficulty scaling unavailable, and reload_config leaves the
        // running scaling untouched rather than silently resetting it to a default.
        std::function<fl::AiScaling(const std::string& preset)> resolveAiScaling;

        // reload_content (#152): evict the content caches and live-apply the changes on the sim thread
        // (flight models re-resolved onto live entities via WorldBroadcaster::reloadFlightModels). The
        // command enqueues onto the sim callback queue; null = content reload unavailable.
        std::function<void()> reloadContent;
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
        // Optional output shell; sim-callback confirmations are also routed here for
        // RCON drain (issue #304). nullptr = disabled.
        CommandShell* shell{nullptr};
    } rcon;

    // Every admin frontend in the process (#1079, D14). admin_unlock and admin_auth_status walk this
    // instead of naming channels through per-frontend hooks, so a channel added later is visible to an
    // operator by construction. Null = no frontends registered, and both commands say so.
    AdminChannelRegistry* adminChannels{nullptr};
};

// Register all fl-server admin commands into registry using the given context.
//
// The context is SHARED, const, and frozen at registration: every handler holds this same pointer,
// so assigning a context field after this call is a compile error rather than a silent no-op. It
// used to be passed by value and deep-copied into each of the ~50 handlers (and again into every
// enqueueSimCallback lambda they build), which made "populate the context, register, then fill in
// one more field" read as working code. #1048 was exactly that, and the field left behind was the
// server's start time, so `status` reported the machine's uptime through all five admin frontends.
void registerServerCommands(CommandRegistry& registry, std::shared_ptr<const ServerCommandContext> ctx);

} // namespace fl
