// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
//
// fl-server — headless dedicated server for fighters-legacy
//
// Configuration (later tiers override earlier ones):
//   1. server.toml in CWD (or path in FL_CONFIG env var)
//   2. CLI positional args: fl-server [port] [maxPeers]
//   3. Environment variables: FL_PORT, FL_BIND_ADDRESS, FL_MAX_PEERS, FL_NAME,
//      FL_PERSISTENT, FL_LOBBY_REGISTER, FL_LOBBY_URL, FL_LOBBY_VISIBILITY,
//      FL_AI_DIFFICULTY_FLOOR  (highest precedence)
//
// See docs/fl-server-config.md for the full operator configuration reference.
// fl-lobby integration is tracked in issue #36.
#include "IpListFile.h"
#include "MissionSource.h"
#include "NetworkFactory.h"
#include "RconServer.h"
#include "ServerCommands.h"
#include "StdoutLogger.h"
#include "TestSpawn.h"
#include "net/DiscoveryBeacon.h"
#include "sensor/SensorDefParser.h"
#include "server_config.h"
#include <content/ContentBootstrap.h>

#include <ILogger.h>
#include <Platform.h>
#include <ai/AiControllerFactory.h>
#include <ai/FormationController.h>
#include <ai/LoiterController.h>
#include <ai/PursuitController.h>
#include <ai/SeatControllers.h> // makeSeatController — crewed-aircraft seat bots (#971)
#include <ai/StateMachineController.h>
#include <ai/Threat.h>
#include <ai/WaypointController.h>
#include <ai/WingmanBehavior.h>
#include <ai/WingmanCommand.h>
#include <atc/AtcBehaviors.h> // makeAtcDepartureController — the ATC departure composition (#706)
#include <atc/AtcService.h>   // the deterministic ATC service (#706)
#include <campaign/CampaignParser.h>
#include <campaign/CampaignRunner.h>
#include <config/ConfigFile.h>
#include <console/CommandRegistry.h>
#include <console/CommandShell.h>
#include <content/AssetManager.h>
#include <content/BundledBaseTerrain.h>
#include <content/ContentIndex.h>
#include <content/ModLoader.h>
#include <difficulty/DifficultyMultipliers.h>
#include <entity/EntityDef.h>
#include <entity/EntityManager.h>
#include <entity/EntityTypeRegistry.h>
#include <flight/BuiltinFlightModel.h> // BuiltinCarrierVesselModel — the builtin carrier's hull (#38)
#include <flight/CentralGravityField.h>
#include <flight/FlightModelParser.h>
#include <flight/LocalFrame.h> // enuBasis — orient an ATC scramble along the runway heading (#706)
#include <job/JobSystem.h>
#include <loop/GameLoop.h>
#include <loop/GameState.h>
#include <mission/BuiltinMissions.h>
#include <mission/Mission.h>
#include <mission/MissionParser.h>
#include <mission/MissionReport.h>
#include <mission/MissionRuntime.h>
#include <mission/MissionSetup.h>
#include <net/GameProtocol.h>
#include <net/WorldBroadcaster.h>
#include <perf/ProcessStats.h>
#include <perf/ServerTickReport.h>
#include <render/BuiltinGeometry.h>
#include <render/RunwaySurfaceMap.h> // surfaceTypeForRunway (#487)
#include <render/TerrainStreamer.h>
#include <script/BuiltinAiScripts.h>
#include <script/LuaController.h>
#include <script/WorldApi.h>
#include <stdfs/StdAsyncFilesystem.h>
#include <stdfs/StdFilesystem.h>
#include <weapon/Loadout.h>
#include <weapon/WeaponRegistry.h>
#include <weather/WeatherController.h>
#include <world/AirportBootstrap.h>
#include <world/AirportRegistry.h>
#include <world/BuiltinAirport.h>
#include <world/FactionRegistry.h>

#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_quit = 0;

static void onSignal(int) {
    g_quit = 1;
}

// ---------------------------------------------------------------------------
// 3-tier config override: CLI positional args + environment variables
// ---------------------------------------------------------------------------

static void applyCliAndEnvOverrides(fl::ServerConfig& cfg, int argc, char** argv, ILogger* log) {
    // Tier 2: CLI positional args — [port] [maxPeers]
    if (argc >= 2 && argv[1][0] != '-')
        cfg.port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc >= 3 && argv[2][0] != '-')
        cfg.maxPeers = std::atoi(argv[2]);

    // Tier 3: environment variables (highest precedence)
    if (const char* e = std::getenv("FL_PORT"))
        cfg.port = static_cast<uint16_t>(std::atoi(e));
    if (const char* e = std::getenv("FL_BIND_ADDRESS"))
        cfg.bindAddress = e;
    if (const char* e = std::getenv("FL_MAX_PEERS"))
        cfg.maxPeers = std::atoi(e);
    if (const char* e = std::getenv("FL_NAME"))
        cfg.name = e;
    if (const char* e = std::getenv("FL_PERSISTENT"))
        cfg.persistent = (std::strcmp(e, "true") == 0 || std::strcmp(e, "1") == 0);
    if (const char* e = std::getenv("FL_LOBBY_REGISTER"))
        cfg.lobbyRegister = (std::strcmp(e, "true") == 0 || std::strcmp(e, "1") == 0);
    if (const char* e = std::getenv("FL_LOBBY_URL"))
        cfg.lobbyUrl = e;
    if (const char* e = std::getenv("FL_LOBBY_VISIBILITY")) {
        if (std::strcmp(e, "public") == 0 || std::strcmp(e, "private") == 0)
            cfg.lobbyVisibility = e;
        else
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "FL_LOBBY_VISIBILITY must be \"public\" or \"private\"; ignoring");
    }
    if (const char* e = std::getenv("FL_AI_DIFFICULTY_FLOOR")) {
        if (std::strcmp(e, "recruit") == 0 || std::strcmp(e, "cadet") == 0 || std::strcmp(e, "veteran") == 0 ||
            std::strcmp(e, "ace") == 0)
            cfg.aiDifficultyFloor = e;
        else
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "FL_AI_DIFFICULTY_FLOOR must be recruit/cadet/veteran/ace; ignoring");
    }
    if (const char* e = std::getenv("FL_OPERATOR_PASSWORD"))
        cfg.operatorPassword = e;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // Pre-pass: --help / --version / --persistent / --bind
    bool flagPersistent = false;
    std::string flagBind;          // non-empty if --bind addr was given
    std::string flagAdminToken;    // non-empty if --admin-token was given (internal single-player use)
    std::string flagTransport;     // non-empty if --transport <gns|enet> was given (overrides [network])
    std::string flagMetricsJson;   // non-empty if --metrics-json path was given (overrides [metrics])
    std::string flagAssets;        // non-empty if --assets <dir> was given (content root; single-player forwards it)
    long flagSimWorkers = -1;      // >=0 if --sim-worker-threads was given (overrides [world])
    long flagFlightSize = -1;      // >=0 if --flight-size was given (overrides [flight] size)
    std::string flagMission;       // non-empty if --mission <name> was given (overrides [rotation])
    std::string flagMissionReport; // non-empty: run the mission headless to completion, write JSON here (#856)
    std::string flagCampaign;      // non-empty if --campaign <file> was given: run the campaign's next sortie (#584)
    std::string flagTimeRate;      // non-empty if --time-rate <name> was given: reduced wall-rate for recording (#915)

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "Usage: fl-server [port] [maxPeers]\n"
                "\n"
                "Options:\n"
                "  --help             Print this message and exit\n"
                "  --version          Print version and exit\n"
                "  --persistent       Enable persistent world mode (Phase 2 -- not yet active)\n"
                "  --bind <addr>      Bind address (overrides server.toml and FL_BIND_ADDRESS)\n"
                "  --assets <dir>     Content root holding mods/ (overrides FL_ASSETS_ROOT and the CWD)\n"
                "  --metrics-json <p> Write the per-phase tick-budget JSON to <p> (overrides [metrics])\n"
                "  --sim-worker-threads <n>  Sim-tick CPU parallelism; 0=auto, 1=serial (overrides [world])\n"
                "  --flight-size <n>         AI wingmen per player; 0=none (overrides [flight])\n"
                "  --mission <name>          Load a mission at startup (overrides [rotation])\n"
                "  --mission-report <path>   Run the mission headless to completion, write a JSON outcome, exit\n"
                "  --time-rate <name>        Sim wall-clock rate: paused|eighth|quarter|half|normal|double|quad|octa\n"
                "                            (sim dt stays 1/60; slows serving for a slow recording client, #915)\n"
                "\n"
                "Admin console commands are available on stdin (type 'help' for a command list).\n"
                "\n"
                "Environment:\n"
                "  FL_CONFIG              Path to server.toml (default: ./server.toml)\n"
                "  FL_PORT                Bind port (default: 4778)\n"
                "  FL_BIND_ADDRESS        Bind address (default: 0.0.0.0)\n"
                "  FL_ASSETS_ROOT         Content root holding mods/ (default: current directory)\n"
                "  FL_MAX_PEERS           Max simultaneous peers (default: 32)\n"
                "  FL_NAME                Server name (default: \"Unnamed Server\")\n"
                "  FL_PERSISTENT          \"true\" to enable persistent world, Phase 2\n"
                "  FL_LOBBY_REGISTER      \"true\" to advertise to fl-lobby, Phase 2\n"
                "  FL_LOBBY_URL           fl-lobby base URL, Phase 2\n"
                "  FL_LOBBY_VISIBILITY    \"public\" or \"private\", Phase 2\n"
                "  FL_AI_DIFFICULTY_FLOOR recruit/cadet/veteran/ace, Phase 2\n"
                "  FL_OPERATOR_PASSWORD    Operator password for network admin commands (overrides server.toml)\n"
                "\n"
                "Config file is written with defaults on first run if absent.\n"
                "See docs/fl-server-config.md for the full operator reference.\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("fl-server %s (%s)\n", "0.0.1", networkBackendVersion(TransportKind::Gns));
            return 0;
        }
        if (std::strcmp(argv[i], "--persistent") == 0)
            flagPersistent = true;
        if (std::strcmp(argv[i], "--bind") == 0 && i + 1 < argc)
            flagBind = argv[++i];
        if (std::strcmp(argv[i], "--transport") == 0 && i + 1 < argc)
            flagTransport = argv[++i];
        if (std::strcmp(argv[i], "--admin-token") == 0 && i + 1 < argc)
            flagAdminToken = argv[++i];
        if (std::strcmp(argv[i], "--metrics-json") == 0 && i + 1 < argc)
            flagMetricsJson = argv[++i];
        if (std::strcmp(argv[i], "--assets") == 0 && i + 1 < argc)
            flagAssets = argv[++i];
        if (std::strcmp(argv[i], "--mission") == 0 && i + 1 < argc)
            flagMission = argv[++i];
        if (std::strcmp(argv[i], "--mission-report") == 0 && i + 1 < argc)
            flagMissionReport = argv[++i];
        if (std::strcmp(argv[i], "--campaign") == 0 && i + 1 < argc)
            flagCampaign = argv[++i];
        if (std::strcmp(argv[i], "--time-rate") == 0 && i + 1 < argc)
            flagTimeRate = argv[++i];
        if (std::strcmp(argv[i], "--sim-worker-threads") == 0 && i + 1 < argc) {
            char* end = nullptr;
            long n = std::strtol(argv[++i], &end, 10);
            if (end != argv[i] && n >= 0 && n <= 256)
                flagSimWorkers = n;
        }
        if (std::strcmp(argv[i], "--flight-size") == 0 && i + 1 < argc) {
            char* end = nullptr;
            long n = std::strtol(argv[++i], &end, 10);
            if (end != argv[i] && n >= 0 && n <= 8)
                flagFlightSize = n;
        }
    }

    // ---- Set up platform ----
    // The network backend is created later (createNetwork), once [network].transport is known.
    Platform p;
    p.logger = std::make_unique<StdoutLogger>();

    ILogger* log = p.logger.get();

    log->log(LogLevel::Info, __FILE__, __LINE__, "fl-server 0.0.1 starting");

    // ---- Tier 1: server.toml ----
    const char* configEnv = std::getenv("FL_CONFIG");
    std::string configPath = configEnv ? configEnv : "server.toml";
    fl::ServerConfig cfg =
        fl::parseServerConfig(fl::ensureAndReadConfig(configPath, fl::defaultServerConfigToml(), *log), log);

    // ---- Tier 2 + 3: CLI positional args and environment variables ----
    applyCliAndEnvOverrides(cfg, argc, argv, log);

    // --persistent / --bind / --admin-token flags from the pre-pass override any lower tier.
    if (flagPersistent)
        cfg.persistent = true;
    if (!flagBind.empty())
        cfg.bindAddress = flagBind;
    // --admin-token takes highest precedence and overrides server.toml + FL_OPERATOR_PASSWORD.
    // Used internally by LocalServer (single-player) to inject a per-session token.
    if (!flagAdminToken.empty())
        cfg.operatorPassword = flagAdminToken;
    // --metrics-json overrides the [metrics] tick_json_path from server.toml.
    if (!flagMetricsJson.empty())
        cfg.metrics.tickJsonPath = flagMetricsJson;
    // --sim-worker-threads overrides the [world] sim_worker_threads from server.toml.
    if (flagSimWorkers >= 0)
        cfg.simWorkerThreads = static_cast<uint32_t>(flagSimWorkers);
    // --flight-size overrides [flight] size. The game client's embedded single-player server passes
    // --flight-size 1, so single-player always flies with a wingman without changing the shipped
    // dedicated-server default (0), which would otherwise move every load-test number.
    if (flagFlightSize >= 0)
        cfg.flight.size = static_cast<uint32_t>(flagFlightSize);

    // ---- Select + create the transport backend (now that [network].transport is known) ----
    // --transport <gns|enet> from the pre-pass overrides the config. Single-player LocalServer passes
    // --transport enet so the enet6 game client and this server match.
    if (!flagTransport.empty())
        cfg.network.transport = flagTransport;
    const TransportKind transportKind = parseTransportKind(cfg.network.transport, TransportKind::Gns);
    p.network = createNetwork(transportKind, log);
    INetwork* net = p.network.get();
    net->setAllowInsecure(cfg.network.allowInsecure);
    net->setNagleTime(cfg.network.gnsNagleTimeUs);
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "transport: %s", networkBackendVersion(transportKind));
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // ---- Phase 2 stub logs ----
    if (cfg.persistent)
        log->log(LogLevel::Info, __FILE__, __LINE__, "persistent world requested (Phase 2 -- not yet active)");

    // Resolve the mission to load at startup (#854): --mission wins, else the first [rotation] item.
    // Multi-item rotation *timing* (advancing to the next item over the running server) lands
    // incrementally; the parse -> load -> sim-setup wiring is the deliverable here. The actual load
    // happens below once the entity registry + weather controller exist (see "load startup mission").
    std::string missionToLoad = flagMission;
    if (missionToLoad.empty() && !cfg.rotationItems.empty())
        missionToLoad = cfg.rotationItems.front();
    if (!cfg.rotationItems.empty() && flagMission.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "rotation: %zu item(s), order=%s (loading first: %s)", cfg.rotationItems.size(),
                      cfg.rotationOrder.c_str(), cfg.rotationItems.front().c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    if (!cfg.modStack.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "mod stack: %zu mod ID(s) configured (explicit ordering not yet active)",
                      cfg.modStack.size());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    if (cfg.lobbyRegister)
        log->log(LogLevel::Info, __FILE__, __LINE__, "lobby registration configured (Phase 2 -- not yet active)");

    // ---- Init network ----
    if (!net->init()) {
        log->log(LogLevel::Error, __FILE__, __LINE__, "network init failed");
        return 1;
    }

    if (!net->bind(cfg.bindAddress.c_str(), cfg.port, cfg.maxPeers)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bind failed: %s", net->getLastError() ? net->getLastError() : "unknown");
        log->log(LogLevel::Error, __FILE__, __LINE__, buf);
        net->shutdown();
        return 1;
    }

    // "listening on" is printed after startup is complete (after primeSpawnHeight and all
    // pre-loop setup), so LocalServer::start() and CI smoke tests that wait for this line
    // only proceed once ENet is actually being serviced by the game loop.
    // Stored here for use after pre-loop setup completes (see below).
    char listeningMsg[192];
    std::snprintf(listeningMsg, sizeof(listeningMsg), "listening on %s:%u (max %d peers) name=\"%s\"",
                  cfg.bindAddress.c_str(), cfg.port, cfg.maxPeers, cfg.name.c_str());

    if (cfg.incomingBandwidthBps || cfg.outgoingBandwidthBps) {
        net->setBandwidthLimit(cfg.incomingBandwidthBps, cfg.outgoingBandwidthBps);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "bandwidth cap: in=%u B/s out=%u B/s", cfg.incomingBandwidthBps,
                      cfg.outgoingBandwidthBps);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    net->setPreHandshakeRateLimit(cfg.preHandshakeRateLimitCount, cfg.preHandshakeWindowMs);

    // ---- LAN discovery beacon ----
    uint8_t discoveryGameModeFlags = 0;
    for (const auto& m : cfg.gameModes) {
        if (m == "campaign")
            discoveryGameModeFlags |= fl::kGameModeCampaign;
        else if (m == "mission")
            discoveryGameModeFlags |= fl::kGameModeMission;
        else if (m == "sandbox")
            discoveryGameModeFlags |= fl::kGameModeSandbox;
    }
    std::unique_ptr<DiscoveryBeacon> beacon;
    if (cfg.discoveryEnabled) {
        DiscoveryBeacon::Config dcfg;
        dcfg.name = cfg.name;
        dcfg.port = cfg.port;
        dcfg.maxPlayers = static_cast<uint8_t>(cfg.maxPeers > 255 ? 255 : cfg.maxPeers);
        dcfg.gameModeFlags = discoveryGameModeFlags;
        dcfg.intervalMs = cfg.discoveryIntervalMs;
        dcfg.broadcastAddr = "255.255.255.255";
        beacon = std::make_unique<DiscoveryBeacon>(dcfg, *log);
        if (!beacon->isOpen()) {
            log->log(LogLevel::Warn, __FILE__, __LINE__, "LAN discovery beacon: no sockets opened; discovery disabled");
            beacon.reset();
        } else {
            log->log(LogLevel::Info, __FILE__, __LINE__, "LAN discovery beacon started");
        }
    }

    // ---- Content system and headless terrain ----
    // Content root resolution mirrors the client (#831) so a single-player pair agrees on where
    // mods/ lives: --assets <dir> > FL_ASSETS_ROOT > the current working directory. LocalServer
    // forwards the client's resolved root via --assets, so the two never disagree.
    namespace fs = std::filesystem;
    fs::path assetsRoot = fs::current_path();
    if (!flagAssets.empty())
        assetsRoot = fs::path(flagAssets);
    else if (const char* ev = std::getenv("FL_ASSETS_ROOT"); ev && *ev)
        assetsRoot = fs::path(ev);
    fs::path userDataRoot = fs::current_path();

    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "content root: %s", assetsRoot.string().c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    p.filesystem = std::make_unique<StdFilesystem>(assetsRoot, userDataRoot);

    {
        auto asyncFs = std::make_unique<StdAsyncFilesystem>(assetsRoot, userDataRoot);
        if (!asyncFs->init()) {
            log->log(LogLevel::Error, __FILE__, __LINE__, "async filesystem init failed");
            return 1;
        }
        p.asyncFilesystem = std::move(asyncFs);
    }

    ModLoader modLoader(*p.filesystem, *log, assetsRoot.string());
    auto packs = modLoader.load();
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "content: %zu mod(s) loaded", packs.size());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // Mount the bundled coarse global base (#474) at lowest priority so the authoritative server
    // serves real-Earth root tiles for physics even with zero user packs (procedural fills finer
    // detail; user packs override). No-op when no base is bundled.
    if (auto base = fl::loadBundledBaseTerrain(*p.filesystem, *log, "base-terrain",
                                               fl::builtinWorldTerrainManifest().terrainId))
        packs.push_back(std::move(base));

    AssetManager assets(std::move(packs), *log);
    assets.initialize(nullptr); // headless — window is null; NeedsConfiguration packs dropped

    fl::TerrainStreamer terrainStreamer(fl::builtinWorldTerrainManifest(), assets, *p.asyncFilesystem, nullptr);
    log->log(LogLevel::Info, __FILE__, __LINE__, "terrain: headless streamer initialized");

    // Apply the configured planet radius BEFORE the first update(): tiles bake curvature and
    // procedural elevations at generation time, so streaming first would generate Earth-radius
    // terrain (and prime wrong spawn elevations) on a non-Earth planet.
    terrainStreamer.setPlanetRadius(cfg.planetRadiusM);

    // Kick off terrain streaming at the origin. A single update() is not enough to guarantee the
    // spawn point's covering tile chain is Ready (procedural loads are rate-limited per update;
    // content-pack tiles load asynchronously), so spawn elevations are primed below via
    // primeSpawnHeight() before they are queried.
    terrainStreamer.update(glm::dvec3(0.0, 0.0, 0.0));

    // ---- Content index (#810) ----
    // Def cross-references (an entity's `sensors`, a hardpoint's `allowed`/`default`) are namespaced
    // IDS, while AssetManager resolves assets by FILENAME STEM. This index reconciles the two, once,
    // and is the only place they meet: without it, `sensors = ["fl-base:apq159"]` builds the path
    // "sensors/fl-base:apq159.toml" and every aircraft in the pack silently flies with no radar.
    fl::ContentIndex contentIndex;
    {
        static constexpr fl::AssetType kIndexedTypes[] = {fl::AssetType::EntityDef, fl::AssetType::SensorDef};
        contentIndex.build(assets, kIndexedTypes, *log);
    }

    // ---- Weapons (#812) ----
    // Registered BEFORE entity defs, so an entity's hardpoints have weapons to resolve against.
    // Keyed by id, so that resolution never touches the filesystem.
    fl::WeaponRegistry weaponRegistry;
    {
        // Builtins first (#440): the sandbox weapons exist in every configuration, pack or not,
        // so the armed debug entity's hardpoints always resolve and the fire path is provable
        // from a bare checkout. The "builtin:" namespace cannot collide with a pack id.
        const uint32_t builtinWeapons = fl::registerBuiltinWeapons(weaponRegistry);
        const uint32_t packWeapons = registerPackWeaponDefs(assets, weaponRegistry, *log);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "content: %u builtin + %u pack weapon(s) registered", builtinWeapons,
                      packWeapons);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // ---- Entity system ----
    fl::EntityTypeRegistry entityRegistry;
    fl::EntityManager entityManager(*log, entityRegistry);

    // The debug entity ships ARMED (#440) — the shared builder keeps server and client identical.
    entityRegistry.registerType(fl::builtinDebugEntityDef());
    // The builtin multi-crew bomber (#966/#977): a pilot + a bot tail-gunner turret, so the whole
    // crew/turret fire path is provable zero-pack (the crewed counterpart to the debug entity).
    entityRegistry.registerType(fl::builtinBomberDef());
    // The ejection parachute (#672): a replicating Effect entity spawned when a pilot ejects. The
    // broadcaster is pointed at it after construction (setParachuteType), below.
    entityRegistry.registerType(fl::builtinParachuteDef());

    // Builtin surface targets + threats (#863): ground/naval/static targets and a SAM site + AAA that
    // shoot back, so the surface categories and air-defense threat exist with zero content mounted.
    {
        const uint32_t surfaceTypes = fl::registerBuiltinSurfaceEntities(entityRegistry);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "content: %u builtin surface entity type(s) registered", surfaceTypes);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // Load content-pack entity definitions into the registry (#683) after the builtin type, so pack
    // types become spawnable via the `spawn` admin command and appear in `types`. MsgEntityTypeDef
    // already ships the populated registry to clients on connect -- no protocol change.
    {
        const uint32_t packTypes = registerPackEntityDefs(assets, entityRegistry, *log);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "content: %u pack entity type(s) registered", packTypes);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    // Projectile entity types (#625): one per flyable weapon, registered NOW because
    // MsgEntityTypeDef only travels in ConnectAck — a type registered after a client connects
    // would reach it as an unresolvable typeIndex.
    {
        const uint32_t projTypes = registerProjectileEntityDefs(weaponRegistry, entityRegistry, *log);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "content: %u projectile type(s) registered", projTypes);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    // Entities are spawned on-demand by WorldBroadcaster::onConnect; none pre-spawned here.

    // ---- Pre-cache peer spawn-point elevations (main-thread only, before gameLoop.start()) ----
    // TerrainStreamer::heightAt() is not thread-safe; all queries must complete before the sim
    // thread starts.
    // Pump terrain streaming until the covering tile chain at (x, z) is Ready to heightReadyAt
    // depth, so heightAt() returns a spawn-accurate elevation rather than a coarse or datum
    // placeholder. Bounded by a wall-clock deadline so a missing/stuck tile can never hang
    // startup. Without this the entity spawns too low and the per-tick floor query snaps it up
    // to the terrain surface once it streams in, which the client sees as the camera jumping a
    // couple seconds after load-in.
    // Spawn config coordinates are planar (x, z) — near-origin theaters. Map a column onto the
    // sphere's NEAR side at radial altitude `agl` above the terrain: solve geodeticAltitude(x,y,z)
    // = terrainRadialElev + agl for the near-side y. General ground queries use the radial
    // heightAt(dvec3) API (#477); this near-side convention stays confined to spawn placement.
    const double planetR = cfg.planetRadiusM;
    auto nearSideSurface = [&](double x, double z, double agl) -> glm::dvec3 {
        const double h = terrainStreamer.heightAt(glm::dvec3{x, 0.0, z}); // radial terrain elevation
        const double target = planetR + h + agl;
        const double rr = target * target - x * x - z * z;
        const double y = rr > 0.0 ? -planetR + std::sqrt(rr) : (h + agl);
        return glm::dvec3{x, y, z};
    };
    // Pump terrain streaming until the covering tile chain above the (x, z) column is Ready to
    // heightReadyAt depth, so the spawn elevation is spawn-accurate rather than a coarse or datum
    // placeholder. Bounded by a wall-clock deadline so a missing/stuck tile can never hang startup.
    // Without this the entity spawns too low and the per-tick floor query snaps it up to the terrain
    // surface once it streams in, which the client sees as the camera jumping after load-in.
    auto primeSpawnHeight = [&](double x, double z) {
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + seconds(5);
        // Pump at the near-side surface estimate, not world-Y 0: far from the origin the near side
        // of the sphere sits well below y=0, and SSE refinement only reaches heightReadyAt depth
        // when the pumped position is close to the surface. The estimate converges as tiles stream.
        while (!terrainStreamer.heightReadyAt(nearSideSurface(x, z, 0.0)) && steady_clock::now() < deadline) {
            terrainStreamer.update(nearSideSurface(x, z, 0.0));
            p.asyncFilesystem->service();
            if (!terrainStreamer.heightReadyAt(nearSideSurface(x, z, 0.0)))
                std::this_thread::sleep_for(milliseconds(2));
        }
        if (!terrainStreamer.heightReadyAt(nearSideSurface(x, z, 0.0)))
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "terrain: spawn-point chunk not ready within timeout; spawning at AGL only");
    };

    // Entity meshes are authored with their origin at the ground-contact point (see
    // gen_builtin_glb.py), so the physics floor can clamp the origin straight to the terrain and
    // the mesh sits ON the ground — no per-mesh clearance offset needed.
    std::vector<std::array<double, 3>> cachedSpawns;
    {
        const double agl = cfg.spawn.aglOffset;
        if (cfg.spawn.points.empty()) {
            // Default spawn: origin.
            constexpr double kDefaultSpawnZ = 0.0;
            primeSpawnHeight(0.0, kDefaultSpawnZ);
            const glm::dvec3 s = nearSideSurface(0.0, kDefaultSpawnZ, agl);
            cachedSpawns.push_back(std::array<double, 3>{s.x, s.y, s.z});
        } else {
            for (const auto& pt : cfg.spawn.points) {
                primeSpawnHeight(pt.x, pt.z);
                const glm::dvec3 s = nearSideSurface(pt.x, pt.z, agl);
                cachedSpawns.push_back(std::array<double, 3>{s.x, s.y, s.z});
            }
        }
    }

    // ---- Airport registry (#699/#486): builtin airfield + pack airports + OurAirports database ----
    // Placed on the sphere before gameLoop.start() and immutable thereafter. Merge order is builtin ->
    // packs -> CSV (first-id-wins, so a pack airport shadows a bundled one of the same id). Near-origin
    // (world-XZ) fields are primed here so their terrain-resolved elevation is accurate. The resolved
    // registry drives runway terrain-flattening via setHeightModifier: the SAME AirportRegistry the
    // client loads (from the same bundled data), so the physics floor, spawn priming, and the tile
    // mesh all flatten identically on both ends.
    fl::AirportRegistry airportRegistry;
    {
        std::vector<fl::AirportDef> airportDefs;
        airportDefs.push_back(fl::builtinAirfield());
        const uint32_t packAirports = fl::registerPackAirportDefs(assets, airportDefs, *log);
        fl::AirportLoadStats csvStats;
        std::vector<fl::AirportDef> csvDefs = fl::loadOrImportAirports(*p.filesystem, *log, &csvStats);
        for (auto& d : csvDefs)
            airportDefs.push_back(std::move(d));
        for (const auto& def : airportDefs) {
            if (def.elevationM < 0.0 && def.useWorldXZ)
                primeSpawnHeight(def.worldX, def.worldZ);
        }
        airportRegistry.load(std::move(airportDefs), planetR,
                             [&terrainStreamer](glm::dvec3 pos) { return terrainStreamer.heightAt(pos); });
        // Runway terrain flattening: gear touches at the authoritative field elevation, and the tile
        // mesh matches the physics floor (both route through TerrainStreamer::heightAt). Set BEFORE any
        // further terrain priming so spawn elevations are taken from already-flattened terrain.
        terrainStreamer.setHeightModifier(
            [&airportRegistry](glm::dvec3 pos, double rawH) { return airportRegistry.flattenedHeight(pos, rawH); },
            [&airportRegistry](glm::dvec3 centre, double radiusM) {
                return airportRegistry.regionHasRunway(centre, radiusM);
            });
        // Runway surface typing (#487): surfaceTypeAt reports the runway surface inside a footprint, so
        // ground physics differentiates a paved runway from the grass beside it.
        terrainStreamer.setSurfaceOverride([&airportRegistry](glm::dvec3 pos) -> std::optional<fl::SurfaceType> {
            const auto s = airportRegistry.runwaySurfaceAt(pos);
            return s ? std::optional<fl::SurfaceType>(fl::surfaceTypeForRunway(*s)) : std::nullopt;
        });
        char buf[144];
        std::snprintf(buf, sizeof(buf), "content: %zu airport(s) loaded (builtin + %u pack + %zu CSV%s)",
                      airportRegistry.count(), packAirports, csvStats.airports,
                      csvStats.csvPresent ? (csvStats.cacheHit ? ", cached" : ", imported") : ", no CSV");
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // ---- WorldBroadcaster wires the sim loop to ENet ----
    fl::WeatherControllerParams wparams;
    wparams.timeScaleRatio = static_cast<float>(cfg.timeScale);
    fl::WeatherController weatherController(wparams);
    // Altitude wind profile (#489): load the [wind] profile_path (relative to the config dir) and
    // apply it, so aircraft feel altitude-dependent wind the client predicts in parity.
    if (!cfg.wind.profilePath.empty()) {
        const fs::path profPath = fs::path(configPath).parent_path() / cfg.wind.profilePath;
        std::ifstream pf(profPath, std::ios::binary);
        if (pf) {
            std::string content((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
            const auto defs = fl::parseWindProfile(content, log);
            std::vector<fl::WeatherController::WindProfileMetKnot> knots;
            for (const auto& d : defs)
                knots.push_back({d.altM, d.speedMs, d.headingDeg});
            if (!knots.empty()) {
                weatherController.setWindProfile(knots);
                log->log(fl::LogLevel::Info, __FILE__, __LINE__, "loaded altitude wind profile");
            }
        } else {
            log->log(fl::LogLevel::Warn, __FILE__, __LINE__, "wind profile_path not found; ignoring");
        }
    }
    fl::WorldBroadcaster broadcaster(entityManager, entityRegistry, *net, *log, &weatherController);
    broadcaster.setParachuteType("builtin:parachute"); // spawn a chute on pilot ejection (#672)
    broadcaster.setAiAutoEject(true);                  // AI pilots punch out when critically hit (#672)
    fl::WorldBroadcasterConfig wbConfig;
    wbConfig.connectRateLimit = cfg.connectRateLimitCount;
    wbConfig.connectRateWindowS = cfg.connectRateLimitWindowS;
    wbConfig.floodMultiplier = cfg.packetFloodMultiplier;
    wbConfig.maxConnectionsPerIp = cfg.maxConnectionsPerIp;
    wbConfig.adminAuthMaxFailures = cfg.adminAuthMaxFailures;
    wbConfig.adminAuthLockoutSeconds = cfg.adminAuthLockoutSeconds;
    wbConfig.motd = cfg.motd;
    wbConfig.motdDisplaySeconds = cfg.motdDisplayS;
    wbConfig.operatorPassword = cfg.operatorPassword;
    wbConfig.playerEntityType = cfg.playerEntityType; // pilot spawn default when client requests none (#834)
    wbConfig.allowObservers = cfg.allowObservers;     // #857
    wbConfig.requiredPacks.clear();                   // #872: parse "id" / "id@version" specs into RequiredPack
    for (const auto& spec : cfg.requiredPacks)
        wbConfig.requiredPacks.push_back(fl::parseRequiredPackSpec(spec));
    wbConfig.requiredPackPolicy =
        fl::parseRequiredPackPolicy(cfg.requiredPackPolicy).value_or(fl::RequiredPackPolicy::Warn);
    wbConfig.idleTimeoutS = cfg.idleTimeoutS;
    wbConfig.drawDistanceKm = static_cast<float>(cfg.drawDistanceKm);
    wbConfig.snapshotBudgetBytes = cfg.snapshotBudgetBytes;
    wbConfig.compressSnapshots = cfg.network.compressSnapshots;
    wbConfig.jitterBufferMaxDepth = cfg.jitterBufferDepth;
    wbConfig.jitterAdaptWindow = cfg.jitterAdaptWindow;
    wbConfig.jitterHysteresis = cfg.jitterHysteresis;
    wbConfig.jitterMultiplier = cfg.jitterMultiplier;
    wbConfig.congestion = fl::makeCongestionParams(cfg.congestionEnabled, cfg.congestionMinSendHz,
                                                   cfg.congestionLossThreshold, cfg.congestionBudgetFloorBytes);
    wbConfig.governor = fl::makeTickGovernorParams(
        cfg.overrunGovernorEnabled, cfg.overrunHighWatermark, cfg.overrunLowWatermark, cfg.overrunMinSnapshotHz,
        cfg.overrunMaxAiStride, cfg.overrunBudgetFloorBytes, cfg.overrunMinInterestFraction);
    wbConfig.gameplay = fl::DamageRules{cfg.friendlyFire, cfg.crashDamage};
    broadcaster.applyConfig(wbConfig);
    // The fire path's vocabulary (#625). After applyConfig, before gameLoop.start(); the registry
    // lives in main's scope and outlives the broadcaster.
    broadcaster.setWeaponRegistry(&weaponRegistry);
    // The first production entity-event consumer (#626): kill attribution, the scoreboard, the
    // kill-feed broadcast, and DamageDef penalties all hang off entity events reaching the
    // broadcaster. Must be registered before gameLoop.start().
    entityManager.addEventHandler(&broadcaster);
    // Planet gravity (terrain curvature was applied to the streamer before the first update()).
    // Function-scope static so lifetime outlasts the broadcaster.
    static fl::CentralGravityField s_gravity{6'371'000.f};
    {
        const auto R_m = static_cast<float>(cfg.planetRadiusM);
        s_gravity = fl::CentralGravityField(R_m);
        broadcaster.setGravityField(s_gravity, R_m / 1000.f);
    }
    // Earth-fixed rotating world frame: Coriolis + centrifugal on every integrator (#482).
    broadcaster.setEarthRotationRate(cfg.earthRotation ? fl::kEarthRotationRate : 0.0);
    // Per-entity terrain height query: sim thread calls heightAt() (thread-safe via shared_mutex).
    // The entity origin is the mesh's ground-contact point, so the floor clamps it directly to the
    // terrain — the mesh then rests ON the ground.
    broadcaster.setGroundElevationQuery(
        [&terrainStreamer](glm::dvec3 pos) { return static_cast<float>(terrainStreamer.heightAt(pos)); });
    // Per-entity ground surface (#487): the runway-surface override + land cover, so the rollout
    // differs by surface. surfaceTypeAt is thread-safe (shared_mutex); the client mirrors it.
    broadcaster.setGroundSurfaceQuery(
        [&terrainStreamer](glm::dvec3 pos) { return terrainStreamer.surfaceTypeAt(pos); });
    // Base operations (#55): the crew chief services an aircraft shut down within a few km of an
    // airport (or on a carrier deck, which WorldBroadcaster checks itself). AirportRegistry is
    // load-once/lock-free, safe to query from the sim thread.
    broadcaster.setBaseProximityQuery([&airportRegistry](glm::dvec3 pos) {
        constexpr double kBaseServiceRangeM = 5000.0;
        return airportRegistry.nearestTo(pos.x, pos.z, kBaseServiceRangeM) != nullptr;
    });
    // Resolve EntityDef::flightModelAsset -> parsed FlightModelData on the spawn path. Loads the raw
    // TOML asset via AssetManager, parses it with engine-flight's parseFlightModel, and caches the
    // result by id (sim-thread-only access). Empty/unknown ids fall back to the builtin model in
    // WorldBroadcaster.
    auto fmCache = std::make_shared<std::unordered_map<std::string, std::shared_ptr<const fl::FlightModelData>>>();
    broadcaster.setFlightModelResolver(
        [&assets, fmCache](const std::string& id) -> std::shared_ptr<const fl::FlightModelData> {
            // The compiled-in carrier's vessel model (#38): a "builtin:" name never touches the
            // filesystem, same rule as every other builtin asset.
            if (id == "builtin:carrier-vessel")
                return fl::BuiltinCarrierVesselModel::get();
            if (auto it = fmCache->find(id); it != fmCache->end())
                return it->second;
            std::shared_ptr<const fl::FlightModelData> model;
            if (auto raw = assets.loadFlightModel(id.c_str()); raw && !raw->bytes.empty()) {
                model = std::make_shared<const fl::FlightModelData>(fl::parseFlightModel(
                    std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size())));
            }
            (*fmCache)[id] = model; // cache misses too, so a bad id isn't re-parsed every connect
            return model;
        });
    // What an entity's DEFAULT loadout costs it in mass and drag (#812). Same injection shape as the
    // resolvers above -- the summation lives in engine-weapon, and engine-net must not link it.
    // Unset would mean every aircraft flies clean, which is exactly the bug this closes.
    broadcaster.setPayloadResolver([&weaponRegistry, log](const fl::EntityDef& def) -> fl::PayloadEffect {
        return fl::defaultPayload(def, weaponRegistry, *log);
    });

    // Build a crewed aircraft's non-fly seat bots (#971). engine-net does not link engine-ai
    // (cmake/layering.cmake), so the concrete seat bots — the turret gunner — reach the broadcaster
    // through this std::function seam, exactly like the flight-order / target-designator hooks below.
    broadcaster.setSeatControllerFactory(
        [&entityManager](const fl::SeatDef& seat, uint8_t seatIdx,
                         const fl::WorldBroadcaster::SeatBotContext& ctx) -> std::unique_ptr<fl::ISeatController> {
            return fl::ai::makeSeatController(seat, seatIdx, entityManager, ctx.skillMin, ctx.skillMax,
                                              ctx.missionSeed);
        });

    // Resolve EntityDef::sensorIds -> parsed SensorDef on the spawn path (#685). A sensor reference
    // is an ID, not an asset name, so it goes through ContentIndex (#810) -- see makeSensorDefResolver.
    broadcaster.setSensorDefResolver(fl::makeSensorDefResolver(assets, contentIndex, *log));
    broadcaster.setSensorCheckHz(static_cast<float>(cfg.sensorCheckHz));

    // ---- Server-side difficulty (#682) -----------------------------------------------------------
    // The FIRST server-side consumer of the difficulty system: until now AiScaling was parsed and
    // round-tripped client-side only, and fl-server knew nothing beyond an `[ai] difficulty_floor`
    // string it never acted on. The sensing pass needs two of its fields in the sim tick —
    // radarSensorRange (how far an AI's radar actually reaches) and reactionTimeS (how long it takes
    // to act on what it saw) — so they arrive here rather than being invented in the engine.
    //
    // The table is mod-overridable (data/difficulty.toml through the AssetManager, highest-priority
    // pack wins) exactly like the client path, so a content pack tunes its own AI without patching
    // the server.
    const fl::DifficultyMultipliers difficultyTable = fl::DifficultyMultipliers::load(assets, *p.filesystem, *log);
    auto resolveAiScaling = [&difficultyTable](const std::string& name) -> fl::AiScaling {
        fl::DifficultyPreset preset = fl::DifficultyPreset::Pilot;
        if (name == "cadet")
            preset = fl::DifficultyPreset::Cadet;
        else if (name == "ace")
            preset = fl::DifficultyPreset::Ace;
        fl::DifficultySettings ds{};
        difficultyTable.applyPreset(preset, ds);
        return ds.ai;
    };
    broadcaster.setAiScaling(resolveAiScaling(cfg.aiDifficulty));
    {
        const fl::AiScaling scaling = resolveAiScaling(cfg.aiDifficulty);
        char buf[192];
        std::snprintf(buf, sizeof(buf), "ai difficulty \"%s\": radar range x%.2f, reaction %.2f s",
                      cfg.aiDifficulty.c_str(), static_cast<double>(scaling.radarSensorRange),
                      static_cast<double>(scaling.reactionTimeS));
        log->log(fl::LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // ---- Formations and the scripted wingman (#610) ----------------------------------------------
    // engine-net does not link engine-ai (cmake/layering.cmake), so the three places an order needs
    // engine-ai — spawning a flight, building a controller, designating a target — are injected here
    // as std::functions. fl-server links both, so this is the seam where they meet.
    broadcaster.setPlayerFaction(cfg.playerFaction);
    broadcaster.setFlightCommandRateLimit(cfg.flight.commandRateLimitPerS);

    if (cfg.flight.size > 0 && cfg.playerFaction == 0) {
        // A flight whose threat logic can never fire is a silently broken feature, not a
        // configuration: areFactionsHostile gives a faction-0 entity no enemies at all.
        log->log(LogLevel::Warn, __FILE__, __LINE__,
                 "[flight] size > 0 but [world] player_faction = 0: players are NEUTRAL, so nothing is "
                 "hostile to them - the wingman's engage/cover orders will never trigger and "
                 "attack_my_target can never designate. Set player_faction = 1.");
    }

    // Formation geometry + behaviour tuning, shared by the spawner and the order handler so a
    // wingman spawned into a slot and a wingman ordered back to it agree on where the slot is.
    fl::ai::WingmanParams wingmanParams{};
    wingmanParams.formation.lateralM = static_cast<float>(cfg.flight.lateralM);
    wingmanParams.formation.aftM = static_cast<float>(cfg.flight.aftM);
    wingmanParams.formation.verticalM = static_cast<float>(cfg.flight.verticalM);
    wingmanParams.engageRangeM = static_cast<float>(cfg.flight.engageRangeM);
    wingmanParams.coverRangeM = static_cast<float>(cfg.flight.coverRangeM);

    if (cfg.flight.size > 0) {
        const std::string flightEntityType = cfg.flight.entityType;
        const uint32_t flightSize = cfg.flight.size;
        broadcaster.setFlightSpawner([&broadcaster, &entityManager, flightEntityType, flightSize,
                                      wingmanParams](uint32_t peerId, fl::EntityId leadEntity) -> fl::FormationId {
            const fl::EntityState* lead = entityManager.get(leadEntity);
            if (!lead)
                return fl::kNoFormation;

            // The player is the anchor AND the commander of their own flight — the common case, but
            // only a special case of the general model (an AWACS commands a flight it is not in).
            const fl::FormationId fid = broadcaster.formations().create("Viper", leadEntity, peerId, fl::kNoFormation);
            if (fid == fl::kNoFormation)
                return fid;

            for (uint32_t slot = 0; slot < flightSize; ++slot) {
                // Spawn each member on its station, so the flight starts formed rather than
                // converging from a pile at the lead's position.
                fl::ai::FormationParams fp = wingmanParams.formation;
                const glm::vec3 off = fl::ai::formationSlotOffset(slot, fp);
                fl::EntityTransform t{};
                std::memcpy(t.quat, lead->transform.quat, sizeof(t.quat));
                // Offsets are in the lead's frame; at spawn the lead is level, so applying them in
                // world axes is close enough — FormationController closes the rest in a second.
                t.pos[0] = lead->transform.pos[0] + static_cast<double>(off.x);
                t.pos[1] = lead->transform.pos[1] + static_cast<double>(off.z);
                t.pos[2] = lead->transform.pos[2] + static_cast<double>(off.y);

                const fl::EntityId id = entityManager.spawn(flightEntityType.c_str(), t);
                if (!id.valid())
                    break;
                // Same faction as the lead: a wingman that is neutral is invisible to hostiles and
                // blind to them in turn.
                if (fl::EntityState* ms = entityManager.get(id)) {
                    ms->factionIndex = lead->factionIndex;
                }

                fl::ai::WingmanParams wp = wingmanParams;
                wp.slotIndex = slot;
                wp.homePoint = glm::dvec3(t.pos[0], t.pos[1], t.pos[2]); // RTB returns to where it started
                broadcaster.registerController(id, fl::ai::makeWingmanController(entityManager, leadEntity,
                                                                                 fl::ai::WingmanCommand::Rejoin,
                                                                                 fl::EntityId{}, wp));

                fl::FormationMember m{};
                m.id = id;
                m.peerId = fl::kNoPeer; // AI: the server owns this aircraft and may retask it
                m.slotIndex = slot;
                broadcaster.formations().addMember(fid, m);
            }
            return fid;
        });
    }

    // Retask one AI member. The formation supplies the anchor (who to form on) and the member its
    // slot, so an order never has to be told where the aircraft belongs.
    broadcaster.setFlightOrderHandler(
        [&broadcaster, &entityManager, wingmanParams](const fl::Formation& formation, const fl::FormationMember& member,
                                                      uint8_t command, fl::EntityId designatedTarget) -> bool {
            if (!fl::ai::isWingmanCommandOrdinal(command))
                return false;
            const auto cmd = static_cast<fl::ai::WingmanCommand>(command);

            fl::ai::WingmanParams wp = wingmanParams;
            wp.slotIndex = member.slotIndex;
            if (const fl::EntityState* ms = entityManager.get(member.id)) {
                // RTB heads for where this aircraft currently is if we never recorded a home; good enough
                // and never NaN. (A real base comes with the mission system, #584.)
                wp.homePoint = glm::dvec3(ms->transform.pos[0], ms->transform.pos[1], ms->transform.pos[2]);
            }

            auto ctrl = fl::ai::makeWingmanController(entityManager, formation.anchor, cmd, designatedTarget, wp);
            if (!ctrl)
                return false;
            broadcaster.registerController(member.id, std::move(ctrl)); // REPLACES the existing controller
            return true;
        });

    // Target designation for attack_my_target. THIS LAMBDA WAS THE SEAM (#610), and the sensor core
    // (#677/#684/#685) has now closed it: the lead designates from ITS OWN CONTACT TABLE — the things
    // it has actually detected, preferring what it has LOCKED — instead of from ground truth.
    //
    // Exactly as promised when the seam was cut, nothing else changed: not the grammar, not the wire,
    // not the client, not the radio menu. One lambda.
    //
    // The boresight-over-ground-truth path remains as the fallback for a server with no sensing
    // evaluated (contactsFor returns null), so a headless/degenerate setup still designates something
    // rather than silently refusing every order.
    {
        const auto designateRangeM = static_cast<float>(cfg.flight.designateRangeM);
        const auto halfAngleRad =
            static_cast<float>(cfg.flight.designateHalfAngleDeg * std::numbers::pi_v<double> / 180.0);
        broadcaster.setTargetDesignator([&broadcaster, &entityManager, designateRangeM, halfAngleRad](
                                            const fl::EntityState& commander, const float viewAxis[3]) -> fl::EntityId {
            if (const fl::sensor::ContactTable* contacts = broadcaster.contactsFor(commander.id.index)) {
                // The honest path: a lead cannot order an attack on something it has not seen. If it
                // is pointing at empty sky, this returns an invalid id and the order is REFUSED
                // ("Two, no joy") rather than quietly retargeted — which is the whole principle.
                return fl::ai::designateFromContacts(commander, viewAxis, contacts, designateRangeM, halfAngleRad);
            }
            return fl::ai::designateBoresightTarget(entityManager, commander, viewAxis, designateRangeM, halfAngleRad,
                                                    &broadcaster.spatialIndex());
        });
    }

    // Wire pre-cached spawn positions. Must be called before gameLoop.start() (never mutated after).
    broadcaster.setSpawnPoints(std::move(cachedSpawns));
    // Seed the physics floor from the already-primed TerrainStreamer at origin.
    // Used by FlightIntegrator::step for ground collision. Updated each frame below.
    // Peer spawn positions are set separately via setSpawnPoints() above.
    primeSpawnHeight(0.0, 0.0); // no-op if already Ready (default-spawn path primed it above)
    broadcaster.setGroundElevation(static_cast<float>(terrainStreamer.heightAt(glm::dvec3{0.0, 0.0, 0.0})));

    // A read-only AI script cache, built before the mission load (so mission `ai: lua <name>` objects
    // can resolve their scripts) and reused later by the ENet admin `spawn --ai lua` path. Safe to read
    // from any thread — it is never mutated after construction, before gameLoop.start().
    std::unordered_map<std::string, std::pair<std::string, std::string>> aiScriptCache;
    // Builtin scripts first (#866): `--ai lua builtin:fighter` and an EntityDef aiScriptAsset =
    // "builtin:fighter" resolve with no pack mounted. Root "" — a builtin uses no require().
    for (std::string_view id : fl::builtinAiScriptIds()) {
        const std::string_view src = fl::builtinAiScript(id);
        if (!src.empty())
            aiScriptCache.emplace(std::string(id),
                                  std::pair<std::string, std::string>{std::string(src), std::string{}});
    }
    for (const auto& name : assets.listAssets(AssetType::AIScript)) {
        auto script = assets.loadAIScript(name.c_str());
        if (!script || script->bytes.empty())
            continue;
        std::string src(script->bytes.begin(), script->bytes.end());
        std::string root = assets.findPackRootForAsset(AssetType::AIScript, name.c_str());
        aiScriptCache.emplace(name, std::pair<std::string, std::string>{std::move(src), std::move(root)});
    }

    // ---- Load the startup mission (#854/#855) ----
    // Resolve the mission asset, hand its bytes to the engine-mission runtime parser (the same schema
    // validate-mission checks), and set up the sim from it BEFORE gameLoop.start(): spawns + factions
    // via applyMission, the coalition registry handed to the broadcaster, and the mission's `player:`
    // objects registered as joinable slots. missionFactions outlives gameLoop (the broadcaster holds a
    // pointer to it), so it is declared here in main's scope.
    fl::FactionRegistry missionFactions;
    // The objective/trigger evaluator (#633). Constructed below when a mission loads; declared here so
    // it outlives gameLoop (the broadcaster's tick hook captures a pointer into it).
    std::unique_ptr<fl::MissionRuntime> missionRuntime;
    // Mission non-terminal `do:` action sink (#212). The evaluator routes actions like `set_weather storm`
    // here; we point it at the admin command dispatch AFTER the registry is built (below), so a mission
    // can do exactly what an operator could and no more. Set on the sim thread; read on the sim thread
    // (the evaluator steps there). Empty until wired = actions are logged-and-skipped.
    std::function<void(std::string_view)> missionActionSink;
    std::string loadedMissionName; // for the #856 headless report
    uint64_t loadedMissionSpawned = 0;

    // The world.* host seam (#413): the engine integration a Lua AI/mission script reaches through
    // world.spawn/despawn/set_relationship/set_music_state/mission_success/mission_failure. Declared
    // before the mission load so its LuaControllers can bind it; the hooks run on the sim thread (from
    // the controller's sample()), where direct EntityManager/FactionRegistry mutation + a music
    // broadcast are all safe. Lifetime spans gameLoop, so it outlives every LuaController.
    fl::WorldApi worldApi;
    worldApi.spawn = [&](const std::string& type, const std::array<double, 3>& pos, float /*heading*/,
                         const std::string& side) -> int {
        fl::EntityTransform t{};
        t.pos[0] = pos[0];
        t.pos[1] = pos[1];
        t.pos[2] = pos[2];
        t.quat[3] = 1.f; // identity; the default LoiterController steers heading from here
        const fl::EntityId id = entityManager.spawn(type.c_str(), t);
        if (!id.valid())
            return -1;
        uint16_t fi = side.empty() ? 0 : missionFactions.indexOf(side);
        if (fi == UINT16_MAX)
            fi = 0;
        if (fl::EntityState* st = entityManager.get(id))
            st->factionIndex = fi;
        // A default loiter keeps a spawned aircraft flying (a bare entity would freeze); a ground unit
        // simply holds. Scripts that want a specific behavior can despawn + respawn with mission bots.
        auto ctrl = std::make_unique<fl::ai::LoiterController>(glm::dvec3(pos[0], pos[1], pos[2]));
        broadcaster.registerController(id, std::move(ctrl), nullptr, fl::kAutoSpawnAirspeed);
        return static_cast<int>(id.index);
    };
    worldApi.despawn = [&](int entityIdx) {
        fl::EntityId target{};
        entityManager.forEach([&](const fl::EntityState& s) {
            if (!s.dead && s.id.index == static_cast<uint32_t>(entityIdx))
                target = s.id;
        });
        if (target.valid())
            entityManager.kill(target);
    };
    worldApi.setRelationship = [&](const std::string& a, const std::string& b, const std::string& rel) {
        const uint16_t ia = missionFactions.indexOf(a);
        const uint16_t ib = missionFactions.indexOf(b);
        if (ia == UINT16_MAX || ib == UINT16_MAX || ia == 0 || ib == 0 || ia == ib)
            return; // unknown/neutral/self — nothing to set
        fl::FactionRelation fr;
        if (rel == "friendly")
            fr = fl::FactionRelation::Friendly;
        else if (rel == "hostile")
            fr = fl::FactionRelation::Hostile;
        else if (rel == "neutral")
            fr = fl::FactionRelation::Neutral;
        else
            return; // unrecognised relation string
        missionFactions.setRelationship(ia, ib, fr);
    };
    worldApi.setMusicState = [&](const std::string& s) {
        fl::GameState gs;
        if (s == "menu")
            gs = fl::GameState::Menu;
        else if (s == "patrol")
            gs = fl::GameState::FlightPatrol;
        else if (s == "combat")
            gs = fl::GameState::FlightCombat;
        else if (s == "success")
            gs = fl::GameState::MissionSuccess;
        else if (s == "debrief")
            gs = fl::GameState::Debrief;
        else
            return; // unknown state name
        broadcaster.broadcastMusicState(static_cast<uint8_t>(gs));
    };
    worldApi.setMissionOutcome = [&](bool success) {
        if (missionRuntime)
            missionRuntime->forceOutcome(success);
    };
    // Haptics (#128): a script's rumble()/rumble_triggers()/stop_rumble() reaches every client, which
    // plays it on its local gamepad. Args are already clamped by the engine binding.
    worldApi.rumble = [&](float low, float high, uint32_t durMs) {
        broadcaster.broadcastHaptic(static_cast<uint8_t>(fl::HapticKind::Rumble), low, high,
                                    static_cast<uint16_t>(durMs));
    };
    worldApi.rumbleTriggers = [&](float left, float right, uint32_t durMs) {
        broadcaster.broadcastHaptic(static_cast<uint8_t>(fl::HapticKind::Triggers), left, right,
                                    static_cast<uint16_t>(durMs));
    };
    worldApi.stopRumble = [&]() {
        broadcaster.broadcastHaptic(static_cast<uint8_t>(fl::HapticKind::Stop), 0.f, 0.f, 0);
    };

    // Campaign mode (#584/#635): resolve the NEXT sortie from the deterministic campaign engine instead
    // of a single fixed mission. The runner ties CampaignEngine to the mission content (loadMissionYaml)
    // and persistence; each server run flies one sortie and, on its outcome, advances the campaign and
    // saves state, so a restart continues the persistent war. The frontline loader is left unset here
    // (raster PNG decoding is content-gated); the campaign still progresses via story injection, dynamic
    // selection, attrition, and set_frontline path tracking.
    std::unique_ptr<fl::CampaignRunner> campaignRunner;
    std::string campaignMissionId;
    std::string campaignSavePath;
    std::optional<std::string> campaignYaml;
    if (!flagCampaign.empty()) {
        if (auto campBytes = fl::loadMissionYaml(flagCampaign, &assets, *log)) {
            fl::CampaignParseResult cp = fl::parseCampaign(*campBytes);
            if (!cp.ok) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "campaign '%.80s' failed to parse (%zu error(s))", flagCampaign.c_str(),
                              cp.errors.size());
                log->log(LogLevel::Error, __FILE__, __LINE__, buf);
                for (const std::string& e : cp.errors)
                    log->log(LogLevel::Error, __FILE__, __LINE__, e.c_str());
            } else {
                // Per-mission/template content resolves through the same loader single missions use.
                auto content = [&assets, log](const std::string& path) -> std::optional<std::string> {
                    return fl::loadMissionYaml(path, &assets, *log);
                };
                uint64_t seed = 1469598103934665603ull; // FNV-1a of the campaign name — stable, replayable
                for (unsigned char ch : cp.campaign.name)
                    seed = (seed ^ ch) * 1099511628211ull;
                std::string sanitized;
                for (char ch : cp.campaign.name)
                    sanitized += (std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
                std::error_code ec;
                std::filesystem::create_directories("cache", ec); // the save dir must exist before writeConfigFile
                campaignSavePath = "cache/campaign_" + sanitized + ".flsave";
                campaignRunner = std::make_unique<fl::CampaignRunner>(std::move(cp.campaign), seed, content);
                if (std::ifstream in{campaignSavePath, std::ios::binary}) {
                    std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    if (campaignRunner->restore(blob))
                        log->log(LogLevel::Info, __FILE__, __LINE__, "campaign: restored saved state");
                }
                campaignYaml = campaignRunner->nextMissionYaml(campaignMissionId);
                if (campaignYaml) {
                    missionToLoad = campaignMissionId.empty() ? std::string("campaign") : campaignMissionId;
                    char buf[192];
                    std::snprintf(buf, sizeof(buf), "campaign '%.64s': flying sortie '%.80s'",
                                  campaignRunner->name().c_str(), campaignMissionId.c_str());
                    log->log(LogLevel::Info, __FILE__, __LINE__, buf);
                } else {
                    log->log(LogLevel::Info, __FILE__, __LINE__,
                             "campaign: no next mission (complete or missing content) — starting empty");
                }
            }
        } else {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "campaign file '%.96s' not found", flagCampaign.c_str());
            log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        }
    }

    if (!missionToLoad.empty()) {
        // Resolution precedence (loadMissionYaml): builtin id (#868, zero-pack) -> a readable
        // .yaml/.yml file path (the authoring loop — iterate a mission without mounting a pack) ->
        // a pack Mission asset. In campaign mode the sortie YAML is already resolved (campaignYaml).
        std::string yaml;
        bool missionFound = false;
        if (campaignYaml) {
            yaml = std::move(*campaignYaml);
            missionFound = true;
        } else if (auto resolved = fl::loadMissionYaml(missionToLoad, &assets, *log)) {
            yaml = std::move(*resolved);
            missionFound = true;
        }
        if (!missionFound) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "mission '%.96s' not found — starting empty", missionToLoad.c_str());
            log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        } else {
            fl::MissionParseResult parsed = fl::parseMission(yaml);
            if (!parsed.ok) {
                char buf[192];
                std::snprintf(buf, sizeof(buf), "mission '%.80s' failed to parse (%zu error(s)) — starting empty",
                              missionToLoad.c_str(), parsed.errors.size());
                log->log(LogLevel::Error, __FILE__, __LINE__, buf);
                for (const std::string& e : parsed.errors)
                    log->log(LogLevel::Error, __FILE__, __LINE__, e.c_str());
            } else {
                // Mission seed (#976): a stable per-mission seed feeding the deterministic per-instance
                // crew skill roll (#971), derived from the mission name so a replay is byte-identical and
                // two missions differ. FNV-1a over the name (0 for an empty name is fine — still stable).
                uint64_t missionSeed = 1469598103934665603ull;
                for (unsigned char ch : parsed.mission.name)
                    missionSeed = (missionSeed ^ ch) * 1099511628211ull;

                // Per-object controller + loadout attachment (#855). engine-mission spawns the object and
                // calls this back with (id, object); here — where engine-ai / engine-script / the weapon
                // registry are available — we turn `route`/`ai` into a controller and `loadout` into an
                // override. route takes precedence over ai when both are present.
                auto onSpawned = [&, missionSeed](fl::EntityId id, const fl::MissionObject& obj) {
                    std::unique_ptr<fl::IEntityController> ctrl;
                    if (!obj.route.empty()) {
                        std::vector<glm::dvec3> wps;
                        wps.reserve(obj.route.size());
                        for (const auto& p : obj.route)
                            wps.emplace_back(p[0], p[1], p[2]);
                        ctrl = std::make_unique<fl::ai::WaypointController>(std::move(wps));
                    } else if (!obj.ai.empty()) {
                        std::vector<std::string> toks;
                        std::istringstream iss(obj.ai);
                        for (std::string tk; iss >> tk;)
                            toks.push_back(tk);
                        if (!toks.empty() && toks[0] == "lua") {
                            const std::string name = toks.size() > 1 ? toks[1] : "";
                            auto cacheIt = aiScriptCache.find(name);
                            if (name.empty() || cacheIt == aiScriptCache.end()) {
                                char m[176];
                                std::snprintf(m, sizeof(m),
                                              "mission ai: lua script '%.72s' for object '%.40s' not found",
                                              name.c_str(), obj.id.c_str());
                                log->log(LogLevel::Warn, __FILE__, __LINE__, m);
                            } else {
                                auto lc = std::make_unique<fl::LuaController>(
                                    cacheIt->second.first, cacheIt->second.second, &entityManager, &worldApi);
                                if (lc->isValid()) {
                                    ctrl = std::move(lc);
                                } else {
                                    char m[224];
                                    std::snprintf(m, sizeof(m), "mission ai: lua script '%.56s' error: %.120s",
                                                  name.c_str(), lc->lastError().c_str());
                                    log->log(LogLevel::Warn, __FILE__, __LINE__, m);
                                }
                            }
                        } else if (!toks.empty()) {
                            std::vector<std::string_view> argViews;
                            for (std::size_t k = 1; k < toks.size(); ++k)
                                argViews.push_back(toks[k]);
                            ctrl = fl::ai::createController(toks[0], std::span<std::string_view>(argViews),
                                                            &entityManager);
                            if (!ctrl) {
                                char m[192];
                                std::snprintf(m, sizeof(m),
                                              "mission ai: unknown behavior or bad args '%.100s' (object '%.30s')",
                                              obj.ai.c_str(), obj.id.c_str());
                                log->log(LogLevel::Warn, __FILE__, __LINE__, m);
                            }
                        }
                    } else {
                        // No explicit ai/route: fall back to the entity type's OWN default AI script, so a
                        // mission's stock aircraft fly themselves without every author restating
                        // `ai: lua <script>` — the same auto-detect the `spawn` admin command does (#634).
                        const fl::EntityDef* def = entityRegistry.findById(obj.type.c_str());
                        if (def && !def->aiScriptAsset.empty()) {
                            auto cacheIt = aiScriptCache.find(def->aiScriptAsset);
                            if (cacheIt != aiScriptCache.end()) {
                                auto lc = std::make_unique<fl::LuaController>(
                                    cacheIt->second.first, cacheIt->second.second, &entityManager, &worldApi);
                                if (lc->isValid())
                                    ctrl = std::move(lc);
                                else {
                                    char m[224];
                                    std::snprintf(m, sizeof(m), "mission ai: default lua script '%.48s' error: %.120s",
                                                  def->aiScriptAsset.c_str(), lc->lastError().c_str());
                                    log->log(LogLevel::Warn, __FILE__, __LINE__, m);
                                }
                            }
                        }
                    }
                    bool registered = false;
                    if (ctrl) {
                        // Initial airspeed (#883/#885): a ground start is parked (0); else the mission's
                        // per-object `speed:` if given, else the engine's sane airborne cruise default — so
                        // a mission bot placed in the air flies instead of tumbling from zero airspeed.
                        const float airspeed =
                            obj.groundStart ? 0.f : (obj.speed ? *obj.speed : fl::kAutoSpawnAirspeed);
                        broadcaster.registerController(id, std::move(ctrl), nullptr, airspeed);
                        registered = true;
                    }

                    // Crew configuration (#976): apply the mission seed + any `crew:` overrides to a
                    // crewed aircraft's seats, so its bot gunners roll per-instance skill within the
                    // configured range. A no-op for a single-seat entity or one with no controller (not a
                    // ControlledEntity). Player slots are handled on the connect path, not here.
                    if (registered && !obj.playerSlot) {
                        fl::WorldBroadcaster::CrewSpawnConfig cc;
                        cc.missionSeed = missionSeed;
                        if (obj.crew) {
                            cc.skillMin = obj.crew->skillMin;
                            cc.skillMax = obj.crew->skillMax;
                            const fl::EntityDef* cdef = entityRegistry.findById(obj.type.c_str());
                            for (const fl::MissionCrewSeat& ms : obj.crew->seats) {
                                int resolved = ms.seatIndex;
                                if (resolved < 0 && cdef) { // name-by-role: resolve to an index
                                    for (std::size_t s = 0; s < cdef->crew.size(); ++s)
                                        if (cdef->crew[s].role == ms.role) {
                                            resolved = static_cast<int>(s);
                                            break;
                                        }
                                }
                                if (resolved < 0 || resolved > 255)
                                    continue; // unresolvable seat/role — validate-mission reports it
                                fl::WorldBroadcaster::CrewSeatSpawnOverride ov;
                                ov.seatIndex = static_cast<uint8_t>(resolved);
                                ov.botSpec = ms.botSpec;
                                ov.skillMin = ms.skillMin;
                                ov.skillMax = ms.skillMax;
                                ov.empty = ms.empty;
                                cc.seats.push_back(std::move(ov));
                            }
                        }
                        broadcaster.applyCrewSpawnConfig(id, cc);
                    }
                    if (!obj.loadout.empty()) {
                        std::vector<std::string> loWarn;
                        if (!broadcaster.setEntityLoadout(id, obj.loadout, loWarn)) {
                            char m[176];
                            std::snprintf(m, sizeof(m),
                                          "mission loadout: object '%.40s' has no controller/registry — ignored",
                                          obj.id.c_str());
                            log->log(LogLevel::Warn, __FILE__, __LINE__, m);
                        }
                        for (const std::string& w : loWarn)
                            log->log(LogLevel::Warn, __FILE__, __LINE__, w.c_str());
                    }
                };

                // Ground-height resolver for `start: ground` objects (#885): the world-Y of the near-side
                // surface at (x, z), so a parked object sits on the terrain rather than at an altitude.
                auto groundHeight = [&](double x, double z) -> double { return nearSideSurface(x, z, 0.0).y; };
                fl::MissionSetupResult setup =
                    fl::applyMission(parsed.mission, entityManager, missionFactions, &weatherController,
                                     cfg.planetRadiusM, onSpawned, groundHeight);
                broadcaster.setFactionRegistry(&missionFactions);

                std::vector<fl::WorldBroadcaster::MissionSpawnSlot> slots;
                slots.reserve(setup.playerSlots.size());
                for (const fl::PlayerSlot& ps : setup.playerSlots) {
                    fl::WorldBroadcaster::MissionSpawnSlot s;
                    s.missionObjectId = ps.id; // so destroy(<slot-id>) binds the pilot's aircraft (#884)
                    s.entityType = ps.type;
                    s.factionIndex = ps.factionIndex;
                    s.pos[0] = ps.pos[0];
                    s.pos[1] = ps.pos[1];
                    s.pos[2] = ps.pos[2];
                    for (int c = 0; c < 4; ++c)
                        s.quat[c] = ps.quat[c];
                    // Initial airspeed for the joining pilot (#883): the slot's `speed:` or the engine's
                    // airborne cruise default, so a mission's airborne player is in stable flight at t=0.
                    s.airspeed = ps.speed ? *ps.speed : fl::kAutoSpawnAirspeed;
                    slots.push_back(std::move(s));
                }
                broadcaster.setMissionPlayerSlots(std::move(slots));

                // Objective / trigger evaluator (#633): runs at the end of each sim tick (second-scale
                // cadence internally). mission_success / mission_failure drive the objective state
                // machine; other `do` actions route through the injected dispatcher (a validated-command
                // seam — logged for now until the mission action grammar is mapped onto the admin path).
                // Seed the objective evaluator with the player slots too, mapped to an INVALID entity so
                // an unoccupied slot reads as "not destroyed" until a pilot claims it (#884). The
                // mission-slot binder below swaps in the pilot's real aircraft on connect.
                std::vector<std::pair<std::string, fl::EntityId>> objectEntities = std::move(setup.objectEntities);
                for (const fl::PlayerSlot& ps : setup.playerSlots)
                    objectEntities.emplace_back(ps.id, fl::EntityId{});

                // Publish the object-id -> entity map to the broadcaster (#914): it is sent as
                // MsgMissionRoster after ConnectAck so a cinematic recorder can resolve entity-relative
                // camera shots. Copy before the map is moved into the MissionRuntime below.
                broadcaster.setMissionRoster(objectEntities);

                missionRuntime = std::make_unique<fl::MissionRuntime>(
                    parsed.mission, std::move(objectEntities), entityManager,
                    [log, &missionActionSink](std::string_view action) {
                        // Route non-terminal `do:` actions (set_weather / set_time / …) through the same
                        // validated command path the admin console uses (#212). The sink is wired to
                        // adminRegistry.dispatch after the registry is built; before that (or with no
                        // operator surface) the action is logged and skipped.
                        if (missionActionSink) {
                            missionActionSink(action);
                        } else {
                            char m[224];
                            std::snprintf(m, sizeof(m), "mission action (no dispatch wired): %.140s",
                                          std::string(action).c_str());
                            log->log(LogLevel::Info, __FILE__, __LINE__, m);
                        }
                    });
                missionRuntime->setOnEnd([log, &broadcaster, &campaignRunner, campaignMissionId,
                                          campaignSavePath](const fl::MissionOutcome& o) {
                    const bool success = o.state == fl::MissionState::Complete;
                    const char* s = success ? "SUCCESS" : "FAILURE";
                    char m[128];
                    std::snprintf(m, sizeof(m), "mission %s after %.1f s (%u trigger(s) fired)", s, o.elapsedSeconds,
                                  o.triggersFired);
                    log->log(LogLevel::Info, __FILE__, __LINE__, m);
                    // Tell clients the real outcome so the debrief stops hardcoding success (#584).
                    broadcaster.broadcastMissionOutcome(
                        static_cast<uint8_t>(success ? fl::MissionResultCode::Success : fl::MissionResultCode::Failure),
                        static_cast<float>(o.elapsedSeconds), static_cast<uint16_t>(o.triggersFired));
                    // Campaign mode (#584/#635): record the sortie's outcome (advancing the frontline /
                    // arming the next story mission) and persist state so a restart flies the next sortie.
                    if (campaignRunner) {
                        campaignRunner->recordOutcome(campaignMissionId, success);
                        fl::writeConfigFile(campaignSavePath, campaignRunner->save(), *log);
                        std::string nextId;
                        const bool more = campaignRunner->nextMissionYaml(nextId).has_value();
                        char cm[176];
                        std::snprintf(cm, sizeof(cm), "campaign: recorded '%.72s' (%s); %s", campaignMissionId.c_str(),
                                      s, more ? "next sortie on restart" : "campaign complete");
                        log->log(LogLevel::Info, __FILE__, __LINE__, cm);
                    }
                });
                broadcaster.setMissionTickHook([rt = missionRuntime.get()](uint64_t t) { rt->step(t); });
                // Bind a pilot's aircraft to its player-slot id on connect (and unbind on disconnect), so
                // destroy(<slot-id>) tracks the live aircraft instead of firing at t=0 (#884). Fired from
                // the handshake on the sim thread — the same thread that steps the runtime.
                broadcaster.setMissionSlotBinder(
                    [rt = missionRuntime.get(), &broadcaster](const std::string& id, fl::EntityId eid) {
                        rt->registerObjectEntity(id, eid);
                        // Advertise the late slot<->aircraft bind so a recorder's mission roster stays current (#914).
                        broadcaster.updateMissionRoster(id, eid);
                    });
                loadedMissionName = parsed.mission.name;
                loadedMissionSpawned = setup.spawned.size();

                for (const std::string& w : setup.warnings)
                    log->log(LogLevel::Warn, __FILE__, __LINE__, w.c_str());
                char buf[224];
                std::snprintf(buf, sizeof(buf),
                              "mission '%.64s' loaded: %zu object(s) spawned, %zu player slot(s), %zu side(s)",
                              parsed.mission.name.c_str(), setup.spawned.size(), setup.playerSlots.size(),
                              parsed.mission.sides.size());
                log->log(LogLevel::Info, __FILE__, __LINE__, buf);
            }
        }
    }

    if (!cfg.trace.inputTraceDir.empty()) {
        broadcaster.setInputTraceDir(cfg.trace.inputTraceDir);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "input tracing enabled -> %s", cfg.trace.inputTraceDir.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    if (!cfg.banlistPath.empty()) {
        auto banned = fl::loadIpListFile(cfg.banlistPath, log);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "banlist: loaded %zu IPs from %s", banned.size(), cfg.banlistPath.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        broadcaster.setBannedAddresses(std::move(banned));
    }
    if (!cfg.allowlistPath.empty()) {
        auto allowed = fl::loadIpListFile(cfg.allowlistPath, log);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "allowlist: loaded %zu IPs from %s", allowed.size(), cfg.allowlistPath.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        broadcaster.setAllowedAddresses(std::move(allowed));
    }
    net->setEventHandler(&broadcaster);

    // ---- Admin command registry (built before gameLoop to satisfy RAII destruction order) ----
    // Destruction order (LIFO): gameLoop first (sim thread stops), then rconServer
    // (RCON I/O thread stops while adminRegistry still alive), then adminShell, then adminRegistry.
    CommandRegistry adminRegistry;
    CommandShell adminShell(*log, adminRegistry);
    // Declared here so rconServer outlives gameLoop but is destroyed before adminRegistry.
    std::unique_ptr<fl::RconServer> rconServer;

    // (The read-only AI script cache is built earlier, above the mission load, so the mission's
    // scripted-bot `ai: lua <name>` objects can resolve their scripts — see aiScriptCache.)

    // Projectile-churn state (#580). Declared BEFORE gameLoop: the churn callback re-enqueues a
    // copy of itself each tick, and those queued copies capture this state by reference — it must
    // outlive the sim thread (LIFO destruction: gameLoop's destructor joins the thread first).
    struct ChurnState {
        double spawnAccum{0.0};                                // fractional spawns/tick remainder
        uint64_t spawnCounter{0};                              // monotonic; drives placement
        std::deque<std::pair<fl::EntityId, uint64_t>> pending; // FIFO of (entity, death tick)
        uint64_t tick{0};                                      // churn-local tick counter
    } churnState;
    std::function<void()> churnTick;

    // Declared before gameLoop so it is destroyed AFTER the loop's sim thread has stopped (the
    // broadcaster holds a raw pointer to it). Constructed + wired below, after gameLoop exists.
    std::unique_ptr<fl::atc::AtcService> atcService;

    GameLoop gameLoop(broadcaster, *log, 60.0, cfg.maxCatchupTicks);

    // Data-parallel sim tick: the worker pool that parallelises the per-entity AI + integrate
    // passes. Constructed before gameLoop.start() and outlives it (declared here in main's scope).
    // 0 = auto (hardware_concurrency), 1 = serial. Injected into the broadcaster below.
    fl::JobSystem jobSystem(cfg.simWorkerThreads);
    broadcaster.setJobSystem(jobSystem);
    {
        char wbuf[96];
        std::snprintf(wbuf, sizeof(wbuf), "sim worker pool: %u background worker(s) (sim_worker_threads=%u)",
                      jobSystem.workerCount(), cfg.simWorkerThreads);
        log->log(LogLevel::Info, __FILE__, __LINE__, wbuf);
    }

    // ---- Air-traffic control (#706) ----
    // Build the ATC service from the airport registry and wire the scramble spawn path. The spawn
    // handler ENQUEUES onto the sim thread (a scramble may originate from a Lua worker thread), spawns
    // a hold-short aircraft oriented down the runway, registers a departure composition that takes off
    // when ATC clears it, and files a takeoff request. Disabled ([atc] enabled=false) = no facilities,
    // and radio commands answer "no ATC available".
    if (cfg.atc.enabled) {
        atcService = std::make_unique<fl::atc::AtcService>(entityManager, airportRegistry, planetR);
        atcService->setSpawnHandler([&](const fl::atc::AtcService::DepartureSpawn& spawn) {
            gameLoop.enqueueSimCallback([&, spawn]() {
                // Orient the airframe along the runway heading at the hold-short point.
                const glm::dvec3 pos = spawn.holdShort.origin;
                const glm::mat3 enu = fl::enuBasis(pos, planetR);
                const double h = static_cast<double>(spawn.holdShort.headingDeg) * std::numbers::pi_v<double> / 180.0;
                const glm::vec3 fwd =
                    glm::vec3(glm::normalize(glm::dvec3(enu[0]) * std::sin(h) + glm::dvec3(enu[1]) * std::cos(h)));
                const glm::vec3 up = glm::vec3(enu[2]);
                const glm::vec3 right = glm::normalize(glm::cross(fwd, up));
                const glm::quat q(glm::mat3(fwd, glm::cross(right, fwd), right));
                fl::EntityTransform t{};
                t.pos[0] = pos.x;
                t.pos[1] = pos.y;
                t.pos[2] = pos.z;
                t.quat[0] = q.x;
                t.quat[1] = q.y;
                t.quat[2] = q.z;
                t.quat[3] = q.w;
                const fl::EntityId id = entityManager.spawn(spawn.typeId.c_str(), t);
                if (!id.valid()) {
                    log->log(LogLevel::Warn, __FILE__, __LINE__, "atc scramble: spawn failed (unknown type?)");
                    return;
                }
                fl::atc::AtcRunwaySpec spec;
                spec.threshold = spawn.runway.threshold;
                spec.fieldCenter = 0.5 * (spawn.runway.threshold + spawn.runway.oppositeEnd);
                spec.headingDeg = spawn.runway.headingDeg;
                spec.runwayElevM = static_cast<float>(spawn.runway.elevationM);
                spec.patternAltM = static_cast<float>(spawn.runway.elevationM) + 600.f;
                fl::ai::ControllerFactory next = [center = spec.fieldCenter, alt = spec.patternAltM]() {
                    return std::make_unique<fl::ai::LoiterController>(center, 4000.f, alt, 0.6f);
                };
                broadcaster.registerController(
                    id, fl::atc::makeAtcDepartureController(*atcService, id, entityManager, spec, std::move(next)),
                    nullptr, /*initialAirspeed=*/0.f); // spawn stationary, holding short
                atcService->requestTakeoff(id, spawn.facilityId);
            });
        });
        broadcaster.setAtcService(atcService.get());
        log->log(LogLevel::Info, __FILE__, __LINE__, "ATC service enabled");
    }

    // ---- Load-test affordance (#573): pre-spawn N server-side AI entities to stress the entity pool
    // + SpatialIndex at scale. A TESTING AFFORDANCE, NOT A CAPACITY GUARANTEE. Disabled (0) by default;
    // must be wired before gameLoop.start() (registerController is sim-thread-only afterward).
    if (cfg.testSpawnAiCount > 0) {
        const double baseElev = terrainStreamer.heightAt(glm::dvec3{0.0, 0.0, 0.0}); // origin chunk primed above
        const double spreadM = cfg.testSpawnSpreadKm * 1000.0;
        const auto positions = fl::testSpawnPositions(cfg.testSpawnAiCount, spreadM, cfg.testSpawnAglM, baseElev);

        // Controller mix (#580): weighted per-index assignment (deterministic, no RNG). Empty mix
        // (the default) = all loiter, keeping the #573 pool+index characterisation baseline intact.
        // Already validated by parseServerConfig — a parse failure here can't happen, but fall back
        // to all-loiter defensively anyway.
        std::vector<fl::TestSpawnMixEntry> mix;
        if (!cfg.testSpawnAiMix.empty()) {
            std::string mixErr;
            if (!fl::parseTestSpawnMix(cfg.testSpawnAiMix, mix, mixErr))
                mix.clear();
        }

        // #980: the load AI's entity type (default the single-seat debug entity; builtin:bomber runs
        // CREWED AI, exercising the per-seat passes + turret replication under scale-gate load).
        const std::string loadType =
            cfg.testSpawnEntityType.empty() ? std::string("builtin:debug-entity") : cfg.testSpawnEntityType;
        uint32_t spawned = 0;
        fl::EntityId prevId; // pursuit/patrol target: the previously spawned load entity
        for (const auto& pos : positions) {
            fl::EntityTransform t{};
            t.pos[0] = pos[0];
            t.pos[1] = pos[1];
            t.pos[2] = pos[2];
            const fl::EntityId id = entityManager.spawn(loadType.c_str(), t);
            if (!id.valid())
                break; // soft cap or unregistered type — stop cleanly

            // Pick the behavior for this index. pursuit/patrol reference the PREVIOUS load entity
            // (a moving target); the very first entity has no predecessor and loiters regardless
            // of the mix. Per-tick AI cost by behavior: loiter = pure guidance math; pursuit =
            // EntityManager::get() on the moving target; patrol = a StateMachineController whose
            // AnyEntityWithinRange transitions run SpatialIndex::queryRadius() EVERY tick in both
            // states (built directly — the patrol_attack factory template's conditions are
            // target-keyed get() lookups, which would not exercise the range-query path #580 is
            // after).
            std::string_view behavior = "loiter";
            if (!mix.empty())
                behavior = fl::assignTestSpawnBehavior(mix, spawned, cfg.testSpawnAiCount);
            std::unique_ptr<fl::IEntityController> ctrl;
            if (behavior == "pursuit" && prevId.valid()) {
                ctrl = std::make_unique<fl::ai::PursuitController>(entityManager, prevId);
            } else if (behavior == "patrol" && prevId.valid()) {
                const glm::dvec3 center(pos[0], pos[1], pos[2]);
                const fl::EntityId target = prevId;
                auto sm = std::make_unique<fl::ai::StateMachineController>(entityManager);
                sm->addState("patrol", [center]() {
                    return std::make_unique<fl::ai::LoiterController>(center, 3000.f, static_cast<float>(center.y));
                });
                sm->addState("investigate", [&entityManager, target]() {
                    return std::make_unique<fl::ai::PursuitController>(entityManager, target);
                });
                // Both transitions run a queryRadius each tick regardless of outcome — the cost is
                // incurred in every state. 2 s dwell prevents thrash at the range boundary.
                sm->addTransition("patrol", "investigate", fl::ai::AnyEntityWithinRange(2000.f), 2.0f);
                sm->addTransition("investigate", "patrol", fl::ai::Not(fl::ai::AnyEntityWithinRange(2500.f)), 2.0f);
                sm->setInitialState("patrol");
                ctrl = std::move(sm);
            }
            if (!ctrl)
                ctrl = std::make_unique<fl::ai::LoiterController>(glm::dvec3(pos[0], pos[1], pos[2]), 3000.f,
                                                                  static_cast<float>(pos[1]));
            broadcaster.registerController(id, std::move(ctrl));
            prevId = id;
            ++spawned;
        }
        char sbuf[224];
        std::snprintf(sbuf, sizeof(sbuf),
                      "test spawn: %u AI entities over %.1f km at %.0f m AGL, mix=%s "
                      "(testing affordance, NOT a capacity guarantee)",
                      spawned, cfg.testSpawnSpreadKm, cfg.testSpawnAglM,
                      cfg.testSpawnAiMix.empty() ? "loiter" : cfg.testSpawnAiMix.c_str());
        log->log(LogLevel::Warn, __FILE__, __LINE__, sbuf);
    }

    // ---- Projectile churn (#580): a self-rearming sim callback spawns testProjectileRate
    // short-lived entities per second and kills each after testProjectileTtlS — sustained
    // spawn+reap traffic through the EntityPool free-list, the O(liveCount) forEach, and the
    // SnapshotDespawn TLV path. Runs on the sim thread (callbacks drain at the top of each tick,
    // before onTick), so spawn/kill need no extra synchronisation. A TESTING AFFORDANCE.
    if (cfg.testProjectileRate > 0.0) {
        const double baseElev = terrainStreamer.heightAt(glm::dvec3{0.0, 0.0, 0.0});
        const double spreadM = cfg.testSpawnSpreadKm * 1000.0;
        const double y = baseElev + cfg.testSpawnAglM;
        const uint64_t ttlTicks = static_cast<uint64_t>(cfg.testProjectileTtlS * 60.0) + 1u;
        const double rate = cfg.testProjectileRate;
        churnTick = [&churnState, &churnTick, &entityManager, &gameLoop, rate, ttlTicks, spreadM, y]() {
            ++churnState.tick;
            // Reap expired projectiles (FIFO: deadlines are monotonic).
            while (!churnState.pending.empty() && churnState.pending.front().second <= churnState.tick) {
                entityManager.kill(churnState.pending.front().first);
                churnState.pending.pop_front();
            }
            // Spawn this tick's quota (fractional accumulator carries sub-tick rates).
            const uint32_t n = fl::churnSpawnCount(churnState.spawnAccum, rate, 1.0 / 60.0);
            for (uint32_t i = 0; i < n; ++i) {
                const auto pos = fl::testProjectilePosition(churnState.spawnCounter++, spreadM, y);
                fl::EntityTransform t{};
                t.pos[0] = pos[0];
                t.pos[1] = pos[1];
                t.pos[2] = pos[2];
                const fl::EntityId id = entityManager.spawn("builtin:debug-entity", t);
                if (!id.valid())
                    break; // soft cap — skip this tick's remainder, retire existing ones first
                churnState.pending.emplace_back(id, churnState.tick + ttlTicks);
            }
            gameLoop.enqueueSimCallback(churnTick); // re-arm for the next tick
        };
        gameLoop.enqueueSimCallback(churnTick);
        char cbuf[160];
        std::snprintf(cbuf, sizeof(cbuf),
                      "test churn: %.1f projectile spawns/s, ttl %.2f s (~%.0f live at steady state; "
                      "testing affordance)",
                      cfg.testProjectileRate, cfg.testProjectileTtlS, cfg.testProjectileRate * cfg.testProjectileTtlS);
        log->log(LogLevel::Warn, __FILE__, __LINE__, cbuf);
    }

    fl::ServerCommandContext adminCtx;
    adminCtx.sim.broadcaster = &broadcaster;
    adminCtx.sim.entityManager = &entityManager;
    adminCtx.sim.typeRegistry = &entityRegistry;
    adminCtx.sim.weatherController = &weatherController;
    adminCtx.sim.worldApi = &worldApi;   // world.* seam for admin-spawned Lua controllers (#413)
    adminCtx.sim.atc = atcService.get(); // atc_status/atc_scramble/atc_hold (#706); null if disabled
    adminCtx.env.beacon = beacon.get();
    adminCtx.sim.gameLoop = &gameLoop;
    adminCtx.env.logger = log;
    adminCtx.env.configPath = &configPath;
    adminCtx.env.quitFlag = &g_quit;
    adminCtx.env.traceDir = cfg.trace.inputTraceDir;
    adminCtx.env.resolveAiScaling = resolveAiScaling;
    adminCtx.env.loadAIScript = [&aiScriptCache](std::string_view name) -> std::pair<std::string, std::string> {
        auto it = aiScriptCache.find(std::string(name));
        return (it != aiScriptCache.end()) ? it->second : std::pair<std::string, std::string>{};
    };
    adminCtx.bans.banlistPath = cfg.banlistPath.empty() ? nullptr : &cfg.banlistPath;
    adminCtx.bans.allowlistPath = cfg.allowlistPath.empty() ? nullptr : &cfg.allowlistPath;
    adminCtx.bans.saveBanlist = [&](const std::unordered_set<std::string>& b) {
        fl::saveIpListFile(cfg.banlistPath, b, log);
    };
    adminCtx.bans.loadBanlist = [&]() { return fl::loadIpListFile(cfg.banlistPath, log); };
    adminCtx.bans.loadAllowlist = [&]() { return fl::loadIpListFile(cfg.allowlistPath, log); };
    adminCtx.shutdown.warningIntervalS = static_cast<uint32_t>(cfg.shutdownWarningIntervalS);
    adminCtx.shutdown.minDelayS = static_cast<uint32_t>(cfg.minShutdownDelayS);
    adminCtx.shutdown.requireConfirm = cfg.shutdownRequireConfirm;
    adminCtx.rcon.shell = &adminShell;
    adminCtx.rcon.clearRconLockout = [&rconServer](const std::string& ip) -> bool {
        return rconServer ? rconServer->clearLockout(ip) : false;
    };
    adminCtx.rcon.getRconAuthSummary = [&rconServer]() -> fl::AuthLockoutSummary {
        return rconServer ? rconServer->getRconAuthSummary() : fl::AuthLockoutSummary{};
    };

    broadcaster.setShutdownCallback([&]() { g_quit = 1; });
    fl::registerServerCommands(adminRegistry, adminCtx);

    // Route mission `do:` actions through the validated admin command path (#212), e.g. a trigger
    // `do: set_weather storm` runs the same set_weather command an operator would. dispatch() is const +
    // thread-safe; mutating commands (set_weather/set_time) enqueue onto the sim callback queue, so this
    // is safe to call from the mission evaluator on the sim thread. The result string is logged.
    missionActionSink = [&adminRegistry, log](std::string_view action) {
        const std::string result = adminRegistry.dispatch(std::string(action));
        char m[288];
        std::snprintf(m, sizeof(m), "mission action '%.120s' -> %.140s", std::string(action).c_str(), result.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, m);
    };

    // MOTD and operator password were applied via applyConfig() above; the admin dispatcher needs
    // adminRegistry (built just above) so it is wired here.
    if (!cfg.operatorPassword.empty()) {
        broadcaster.setAdminDispatch([&adminRegistry](std::string_view cmd) { return adminRegistry.dispatch(cmd); });
        broadcaster.setAdminShell([&adminShell]() { return adminShell.mark(); },
                                  [&adminShell](int m) { return adminShell.drainSince(m); });
        log->log(LogLevel::Info, __FILE__, __LINE__, "network admin commands: enabled");
    } else {
        log->log(LogLevel::Info, __FILE__, __LINE__,
                 "network admin commands: disabled (no operator_password configured)");
    }

    // ---- Signal handling ----
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    adminCtx.env.startTime = std::chrono::steady_clock::now();

    // ---- Headless mission run-to-completion (#856) ----
    // With --mission-report, step the sim in a deterministic fixed-step loop (no wall-clock, no sim
    // thread, no networking wait) until the mission ends or a tick cap, write a JSON outcome, and exit.
    // The overrun governor is pinned OFF so AI decimation (aiStride > 1) cannot make the outcome
    // load-dependent — determinism is the entire point of a mission-as-test. Combined with pure Lua
    // scripts (no wall-clock / unseeded RNG), a run is byte-reproducible.
    if (!flagMissionReport.empty()) {
        if (!missionRuntime) {
            log->log(LogLevel::Error, __FILE__, __LINE__, "--mission-report requires a mission (use --mission <name>)");
            return 2;
        }
        broadcaster.setGovernorParams(fl::makeTickGovernorParams(false, 0.9f, 0.6f, 15.f, 4, 400));
        constexpr double kSimDt = 1.0 / 60.0;
        constexpr uint64_t kMaxReportTicks = 36000; // 10 sim-minutes at 60 Hz; a stuck mission stops here
        uint64_t ranTicks = 0;
        for (uint64_t tick = 1; tick <= kMaxReportTicks; ++tick) {
            broadcaster.onTick(kSimDt, tick);
            ranTicks = tick;
            if (missionRuntime->done())
                break;
        }
        const fl::MissionOutcome& oc = missionRuntime->outcome();
        fl::MissionReport rep;
        rep.missionName = loadedMissionName;
        rep.outcome = oc.state == fl::MissionState::Complete ? "success"
                      : oc.state == fl::MissionState::Failed ? "failure"
                                                             : "incomplete";
        rep.elapsedSeconds = oc.elapsedSeconds;
        rep.ticks = ranTicks;
        rep.triggersFired = oc.triggersFired;
        rep.liveEntities = entityManager.liveCount();
        rep.spawnedObjects = loadedMissionSpawned;
        fl::writeConfigFile(flagMissionReport, fl::toJson(rep) + "\n", *log);
        char rbuf[256];
        std::snprintf(rbuf, sizeof(rbuf), "mission report: %s after %.1f s / %llu tick(s) -> %s", rep.outcome.c_str(),
                      rep.elapsedSeconds, static_cast<unsigned long long>(rep.ticks), flagMissionReport.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, rbuf);
        return 0;
    }

    // ---- Start sim loop ----
    // Emit the "listening on" line now that pre-loop setup (including primeSpawnHeight) is done.
    // LocalServer::start() waits for this line; emitting it here ensures ENet is serviced before
    // the client attempts its first connection.
    log->log(LogLevel::Info, __FILE__, __LINE__, listeningMsg);
    gameLoop.start();

    // ---- Reduced wall-clock rate for cinematic recording (#915) ----
    // sim dt stays 1/60 — content is byte-identical; ticks just arrive slower, so a slow (software-
    // rendered, lavapipe) recording client never misses a capture boundary. Applied after start()
    // because setRate is a main-thread control (see GameLoop).
    if (!flagTimeRate.empty()) {
        auto parseTimeRate = [](const std::string& s, TimeRate& out) -> bool {
            if (s == "paused")
                out = TimeRate::Paused;
            else if (s == "eighth")
                out = TimeRate::Eighth;
            else if (s == "quarter")
                out = TimeRate::Quarter;
            else if (s == "half")
                out = TimeRate::Half;
            else if (s == "normal")
                out = TimeRate::Normal;
            else if (s == "double")
                out = TimeRate::Double;
            else if (s == "quad")
                out = TimeRate::Quad;
            else if (s == "octa")
                out = TimeRate::Octa;
            else
                return false;
            return true;
        };
        TimeRate tr = TimeRate::Normal;
        if (parseTimeRate(flagTimeRate, tr)) {
            gameLoop.setRate(tr);
            log->log(LogLevel::Info, __FILE__, __LINE__, ("time-rate set to " + flagTimeRate).c_str());
        } else {
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     ("--time-rate: unknown rate \"" + flagTimeRate +
                      "\" (paused|eighth|quarter|half|normal|double|quad|octa); using normal")
                         .c_str());
        }
    }

    // ---- RCON server (optional TCP remote admin channel) ----
    if (cfg.rcon.enabled) {
        rconServer = std::make_unique<fl::RconServer>(adminRegistry, cfg.rcon, *log, &adminShell);
        if (!rconServer->start()) {
            log->log(LogLevel::Warn, __FILE__, __LINE__, "RCON server failed to start; continuing without RCON");
            rconServer.reset();
        }
    }

    // ---- Admin console (stdin command loop) ----
    std::mutex stdinMutex;
    std::queue<std::string> stdinLines;
    std::thread([&stdinMutex, &stdinLines]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> lk(stdinMutex);
            stdinLines.push(std::move(line));
        }
    }).detach();

    // Per-phase tick-budget JSON export (atomic .tmp -> rename each interval). Disabled when path empty.
    const std::string metricsPath = cfg.metrics.tickJsonPath;
    const auto metricsInterval = std::chrono::milliseconds(cfg.metrics.tickJsonIntervalMs);
    auto nextMetricsWrite = std::chrono::steady_clock::now();
    if (!metricsPath.empty()) {
        char mbuf[256];
        std::snprintf(mbuf, sizeof(mbuf), "writing tick-budget metrics to %s every %u ms", metricsPath.c_str(),
                      cfg.metrics.tickJsonIntervalMs);
        log->log(LogLevel::Info, __FILE__, __LINE__, mbuf);
    }

    uint64_t lastDroppedTicks = 0; // for the sim-overrun drop-rate Warn (#514)
    // Baseline RSS captured once after all init, before the main loop; the soak leak gate tracks
    // the growth (rss_kb - rss_startup_kb) over the run (#707). 0 when unavailable on this platform.
    const uint64_t rssStartupKb = fl::currentRssKb();
    while (!g_quit) {
        {
            std::lock_guard<std::mutex> lk(stdinMutex);
            while (!stdinLines.empty()) {
                std::string line = std::move(stdinLines.front());
                stdinLines.pop();
                std::string result = adminRegistry.dispatch(line);
                if (!result.empty())
                    std::printf("[admin] %s\n", result.c_str());
            }
        }
        if (beacon)
            beacon->tick(broadcaster.getPeerCount());
        p.asyncFilesystem->service();
        // Follow the entity so terrain chunks are loaded at its current position.
        const double entityX = broadcaster.cachedEntityX();
        const double entityZ = broadcaster.cachedEntityZ();
        terrainStreamer.update(glm::dvec3(entityX, 0.0, entityZ));

        // Sim-overrun drop signal (#514): when the GameLoop catch-up cap discards ticks, the sim is
        // falling behind even after the governor sheds work — surface it. Checked each ~50 ms poll.
        const uint64_t droppedTicks = gameLoop.totalDroppedTicks();
        if (droppedTicks > lastDroppedTicks) {
            char dbuf[96];
            std::snprintf(dbuf, sizeof(dbuf), "sim overrun: %llu tick(s) dropped (total %llu)",
                          static_cast<unsigned long long>(droppedTicks - lastDroppedTicks),
                          static_cast<unsigned long long>(droppedTicks));
            log->log(LogLevel::Warn, __FILE__, __LINE__, dbuf);
            lastDroppedTicks = droppedTicks;
        }

        if (!metricsPath.empty() && std::chrono::steady_clock::now() >= nextMetricsWrite) {
            const fl::OverrunStatus ov = broadcaster.getOverrunStatus();
            const fl::CongestionTelemetry ct = broadcaster.getCongestionTelemetry();
            const fl::WireTelemetry wt = broadcaster.getWireTelemetry(); // #772 socket bytes, not payload
            const fl::ServerTickReport rep = fl::makeServerTickReport(
                broadcaster.getTickBudget(), broadcaster.getPeerCount(), entityManager.liveCount(), ov.loadFactor,
                droppedTicks, fl::currentRssKb(), rssStartupKb, ov.interestScale, ct.minSendHz, ct.recoveredSendHz,
                ct.maxPacketLoss, wt.outKbs, wt.inKbs, wt.outPacketsPerSec, wt.peersAtSample);
            fl::writeConfigFile(metricsPath, fl::toJson(rep) + "\n", *log);
            nextMetricsWrite = std::chrono::steady_clock::now() + metricsInterval;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (rconServer)
        rconServer->stop();
    gameLoop.stop();
    p.asyncFilesystem->shutdown(); // join worker thread before TerrainStreamer destructs

    log->log(LogLevel::Info, __FILE__, __LINE__, "shutting down");
    net->disconnect();
    net->shutdown();

    return 0;
}
