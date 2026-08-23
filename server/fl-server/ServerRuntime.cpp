// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServerRuntime.h"

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
//      FL_LOBBY_REGISTER, FL_LOBBY_URL, FL_LOBBY_VISIBILITY,
//      FL_AI_DIFFICULTY_FLOOR  (highest precedence)
//
// See docs/server-ops/server-config.md for the full operator configuration reference.
// Lobby registration ships (#143); the reference lobby service is #999.
#include "GameModeSource.h"
#include "HttpAdminServer.h"
#include "IpListFile.h"
#include "MatchTeams.h"
#include "MissionSource.h"
#include "NetworkFactory.h"
#include "RconServer.h"
#include "ReplayRecorder.h"
#include "ServerCommands.h"
#include "StdinCommandReader.h"
#include "StdoutLogger.h"
#include "TestSpawn.h"
#include "WorldStateBridge.h"
#include "bots.h"
#include "http/CurlHttpClientFactory.h" // #143 lobby registration HTTP backend
#include "match/MatchController.h"
#include "net/DiscoveryBeacon.h"
#include "net/LobbyRegistration.h"
#include "net/ServerQueryResponder.h"
#include "sensor/SensorDefParser.h"
#include "server_config.h"
#include <content/ContentBootstrap.h>

#include "Version.h" // FL_VERSION_STRING — stamped into every recording (#643)

#include "AiControllerBuild.h" // the one AI-controller construction ladder (#1236)
#include <ILogger.h>
#include <Platform.h>
#include <ai/AiControllerFactory.h>
#include <ai/ChatIntentBridge.h> // free-text wingman commands over team chat (#611)
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
#include <campaign/FrontlinePng.h>
#include <campaign/TheaterManifest.h>
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
#include <flight/FlightModelLoad.h>
#include <flight/LocalFrame.h> // enuBasis — orient an ATC scramble along the runway heading (#706)
#include <job/JobSystem.h>
#include <loop/GameLoop.h>
#include <loop/GameState.h>
#include <math/Fnv.h> // the one FNV-1a (#1247)
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
#include <stdfs/StdFilesystemWatcher.h>
#include <weapon/Loadout.h>
#include <weapon/WeaponRegistry.h>
#include <weather/WeatherController.h>
#include <world/AirportBootstrap.h>
#include <world/AirportRegistry.h>
#include <world/AlertSystem.h>
#include <world/BuiltinAirport.h>
#include <world/EscalationPolicy.h>
#include <world/FactionRegistry.h>
#include <world/NullAiProvider.h> // the no-provider path (#163)
#include <world/SandboxHome.h>    // the sandbox home — the anchor planar spawn coordinates are measured from (#1211)
#include <world/WorldAiProviderHost.h> // provider loading + WorldEvolutionDelta application (#163)

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
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace fl;

// The fixed sim rate. Named because two places need to agree on it: the GameLoop that drives the
// tick, and the end-of-tick hook that hands sim systems their timestep.
// Derived, not re-declared (#1253/#1075): TickRate.h is where "where is 60 decided" is
// answered, and this used to be a second answer that could drift from it silently.
static constexpr double kSimTickRateHz = static_cast<double>(fl::kServerTickRate.hz());

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
        cfg.server.port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc >= 3 && argv[2][0] != '-')
        cfg.server.maxPeers = std::atoi(argv[2]);

    // Tier 3: environment variables (highest precedence)
    if (const char* e = std::getenv("FL_PORT"))
        cfg.server.port = static_cast<uint16_t>(std::atoi(e));
    if (const char* e = std::getenv("FL_BIND_ADDRESS"))
        cfg.server.bindAddress = e;
    if (const char* e = std::getenv("FL_MAX_PEERS"))
        cfg.server.maxPeers = std::atoi(e);
    if (const char* e = std::getenv("FL_NAME"))
        cfg.server.name = e;
    if (const char* e = std::getenv("FL_LOBBY_REGISTER"))
        cfg.lobby.registerServer = (std::strcmp(e, "true") == 0 || std::strcmp(e, "1") == 0);
    if (const char* e = std::getenv("FL_LOBBY_URL"))
        cfg.lobby.url = e;
    if (const char* e = std::getenv("FL_LOBBY_VISIBILITY")) {
        if (std::strcmp(e, "public") == 0 || std::strcmp(e, "private") == 0)
            cfg.lobby.visibility = e;
        else
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "FL_LOBBY_VISIBILITY must be \"public\" or \"private\"; ignoring");
    }
    if (const char* e = std::getenv("FL_AI_DIFFICULTY_FLOOR")) {
        if (std::strcmp(e, "recruit") == 0 || std::strcmp(e, "cadet") == 0 || std::strcmp(e, "veteran") == 0 ||
            std::strcmp(e, "ace") == 0)
            cfg.ai.difficultyFloor = e;
        else
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "FL_AI_DIFFICULTY_FLOOR must be recruit/cadet/veteran/ace; ignoring");
    }
    if (const char* e = std::getenv("FL_OPERATOR_PASSWORD"))
        cfg.security.operatorPassword = e;
}

namespace fs = std::filesystem;

namespace fl {

// ---------------------------------------------------------------------------
// ServerRuntime::Impl — every owned object, in TEARDOWN ORDER
//
// Declaration order here IS the destruction contract, because destruction is reverse declaration
// order. That order is deliberately the same order these objects were declared in the old main():
// the sequence that works is the one already running, and #1084 is about making it language-
// enforced rather than a comment. It encodes two hazards that have each already produced a bug --
// the stdin reader and the replay recorder are destroyed BEFORE the game loop (they are declared
// after it, #1038), and voice capture is torn down before SDL_Quit() (#1054).
//
// An object whose constructor arguments are only known during a phase is held by unique_ptr and
// built there; the phase binds `auto& name = *m_name;` so the moved code reads unchanged. The rest
// default-construct and are assigned into.
// ---------------------------------------------------------------------------
struct ServerRuntime::Impl {
    // Projectile-churn state (#580). The churn callback re-enqueues a copy of itself each tick and
    // those queued copies capture this by reference, so it must outlive the sim thread.
    struct ChurnState {
        double spawnAccum{0.0};                                // fractional spawns/tick remainder
        uint64_t spawnCounter{0};                              // monotonic; drives placement
        std::deque<std::pair<fl::EntityId, uint64_t>> pending; // FIFO of (entity, death tick)
        uint64_t tick{0};                                      // churn-local tick counter
    };

    explicit Impl(ServerRuntime::Options& o) : m_opts(o) {}

    ServerRuntime::Options& m_opts;
    int m_exitCode{0};

    const fl::ServerUptime m_serverUptime;
    Platform m_p;
    ILogger* m_log{nullptr};
    std::string m_configPath;
    fl::ServerConfig m_cfg;
    INetwork* m_net{nullptr};
    std::string m_missionToLoad;
    std::string m_modeRefToLoad;
    char m_listeningMsg[192]{};
    std::unique_ptr<fl::ServerQueryResponder> m_queryResponder;
    std::unique_ptr<DiscoveryBeacon> m_beacon;
    std::unique_ptr<IHttpClient> m_httpClient;
    std::unique_ptr<LobbyRegistration> m_lobbyReg;
    std::filesystem::path m_assetsRoot;
    std::filesystem::path m_userDataRoot;
    std::unique_ptr<ModLoader> m_modLoader;
    std::unique_ptr<AssetManager> m_assets;
    fl::GameModeDef m_gameMode;
    std::unique_ptr<fl::TerrainStreamer> m_terrainStreamer;
    fl::ContentIndex m_contentIndex;
    fl::WeaponRegistry m_weaponRegistry;
    fl::EntityTypeRegistry m_entityRegistry;
    std::unique_ptr<fl::EntityManager> m_entityManager;
    double m_planetR{0.0};
    std::function<glm::dvec3(double, double, double)> m_nearSideSurface;
    std::function<bool(double, double, std::chrono::steady_clock::time_point)> m_primeSpawnHeightUntil;
    std::function<void(double, double)> m_primeSpawnHeight;
    std::vector<std::array<double, 3>> m_cachedSpawns;
    fl::AirportRegistry m_airportRegistry;
    fl::WeatherControllerParams m_wparams;
    std::unique_ptr<fl::WeatherController> m_weatherController;
    // ⚑ Declared BEFORE the broadcaster, so they are destroyed after it (#1082): the broadcaster
    // takes the ENet admin channel as a raw pointer at construction and dispatches through it for as
    // long as it lives, and the channel in turn dispatches into the registry.
    CommandRegistry m_adminRegistry;
    std::unique_ptr<fl::AdminChannel> m_enetChannel;
    std::unique_ptr<fl::WorldBroadcaster> m_broadcaster;
    // ⚑ Default-initialized HERE, not in an init phase (#1178): initWorld's queries.flightModel
    // resolver and initAdmin's reloadContent hook both capture this shared_ptr BY VALUE, so it must
    // already point at the one map when those lambdas are built. A phase-ordered make_shared ran
    // after the captures — the resolver held a null copy and the first pack aircraft spawn crashed
    // the server; a later one would silently hand resolver and reload two DIFFERENT maps.
    using FlightModelCache = std::unordered_map<std::string, std::shared_ptr<const fl::FlightModelData>>;
    std::shared_ptr<FlightModelCache> m_fmCache = std::make_shared<FlightModelCache>();
    std::unique_ptr<fl::DifficultyMultipliers> m_difficultyTable;
    std::function<fl::AiScaling(const std::string&)> m_resolveAiScaling;
    fl::ai::WingmanParams m_wingmanParams{};
    std::unordered_map<std::string, std::pair<std::string, std::string>> m_aiScriptCache;
    fl::FactionRegistry m_missionFactions;
    std::unique_ptr<fl::AlertSystem> m_alertSystem;
    std::unique_ptr<fl::MissionRuntime> m_missionRuntime;
    fl::MatchController m_matchController;
    std::size_t m_rotationIndex{0};
    std::unique_ptr<fl::BotRoster> m_botRoster;
    std::function<void(std::string_view)> m_missionActionSink;
    // Late-bound seams (#1082). The broadcaster takes its hooks at construction, but these four are
    // only decided once the mission and the recorder exist, so the installed hook forwards to one of
    // these and each reproduces exactly what an UNSET hook used to mean when it is empty.
    std::function<void(const std::string&, fl::EntityId)> m_missionSlotBind;
    std::function<std::optional<uint16_t>(uint32_t)> m_teamAssign;
    std::function<bool(uint32_t, uint16_t)> m_teamSwitchAllowed;
    std::function<void(const fl::ReplayTickRecords&)> m_replayTap;
    std::string m_loadedMissionName;
    uint64_t m_loadedMissionSpawned{0};
    fl::WorldApi m_worldApi;
    std::unique_ptr<fl::CampaignRunner> m_campaignRunner;
    std::string m_campaignMissionId;
    std::string m_campaignSavePath;
    std::optional<std::string> m_campaignYaml;
    std::unique_ptr<fl::WorldStateBridge> m_worldStateBridge;
    std::unique_ptr<CommandShell> m_adminShell;
    fl::AdminChannelRegistry m_adminChannels;
    std::unique_ptr<fl::AdminChannel> m_stdinChannel;
    std::unique_ptr<fl::AdminChannel> m_missionChannel;
    std::unique_ptr<fl::AdminChannel> m_rconChannel;
    std::unique_ptr<fl::AdminChannel> m_httpChannel;
    std::unique_ptr<fl::RconServer> m_rconServer;
    std::unique_ptr<fl::HttpAdminServer> m_httpAdminServer;
    std::function<void()> m_churnTick;
    std::unique_ptr<fl::atc::AtcService> m_atcService;
    std::unique_ptr<GameLoop> m_gameLoop;
    std::unique_ptr<fl::JobSystem> m_jobSystem;
    std::shared_ptr<const fl::ServerCommandContext> m_adminCtx;
    fl::ReplayRecorder m_replayRecorder;
    fl::WorldEvolutionSinks m_aiSinks;
    fl::StdinCommandReader m_stdinReader;
    ChurnState m_churnState;

    // The phases, in call order.
    [[nodiscard]] bool initConfig();
    [[nodiscard]] bool initNet();
    [[nodiscard]] bool initContent();
    [[nodiscard]] bool initWorld();
    [[nodiscard]] bool initMission();
    [[nodiscard]] bool initAdmin();
    [[nodiscard]] bool initSystems();
    [[nodiscard]] int mainLoop();
};

bool ServerRuntime::Impl::initConfig() {
    // The members this phase touches, bound by name so the moved code reads unchanged --
    // and so its `[&name]` lambda captures still compile: you cannot capture a member.
    [[maybe_unused]] auto& cfg = m_cfg;
    [[maybe_unused]] auto& configPath = m_configPath;
    [[maybe_unused]] auto& log = m_log;
    [[maybe_unused]] auto& missionToLoad = m_missionToLoad;
    [[maybe_unused]] auto& modeRefToLoad = m_modeRefToLoad;
    [[maybe_unused]] auto& net = m_net;
    [[maybe_unused]] auto& p = m_p;
    // ---- Set up platform ----
    // The network backend is created later (createNetwork), once [network].transport is known.
    // p is a member (see Impl)
    p.logger = std::make_unique<StdoutLogger>();

    log = p.logger.get();

    // The real build version, not a literal (#1049): this is the first line of every server log and
    // the one an operator reads to confirm which build is deployed. It said 0.0.1 while --version and
    // the MCP serverInfo both reported the truth.
    {
        char startMsg[96];
        std::snprintf(startMsg, sizeof(startMsg), "fl-server %s starting", FL_VERSION_STRING);
        log->log(LogLevel::Info, __FILE__, __LINE__, startMsg);
    }

    // ---- Tier 1: server.toml ----
    const char* configEnv = std::getenv("FL_CONFIG");
    configPath = configEnv ? configEnv : "server.toml";
    cfg = fl::parseServerConfig(fl::ensureAndReadConfig(configPath, fl::defaultServerConfigToml(), *log), log);

    // ---- Tier 2 + 3: CLI positional args and environment variables ----
    applyCliAndEnvOverrides(cfg, m_opts.argc, m_opts.argv, log);

    // --bind / --admin-token flags from the pre-pass override any lower tier.
    if (!m_opts.bind.empty())
        cfg.server.bindAddress = m_opts.bind;
    // --admin-token takes highest precedence and overrides server.toml + FL_OPERATOR_PASSWORD.
    // Used internally by LocalServer (single-player) to inject a per-session token.
    if (!m_opts.adminToken.empty())
        cfg.security.operatorPassword = m_opts.adminToken;
    // --metrics-json overrides the [metrics] tick_json_path from server.toml.
    if (!m_opts.metricsJson.empty())
        cfg.metrics.tickJsonPath = m_opts.metricsJson;
    // The embedded single-player server is told where to record (#41): its working directory is
    // wherever the CLIENT was launched from, which is not where the replay browser looks.
    if (!m_opts.replayDir.empty())
        cfg.replay.dir = m_opts.replayDir;
    // Both of these exist for the determinism gate (#644), which needs a populated world and the
    // recorder's own per-tick hashes without hand-editing a server.toml in a ctest.
    if (!m_opts.replayHashLog.empty())
        cfg.replay.hashLog = m_opts.replayHashLog;
    if (m_opts.testSpawnAi >= 0)
        cfg.world.testSpawnAiCount = static_cast<uint32_t>(m_opts.testSpawnAi);
    // --sim-worker-threads overrides the [world] sim_worker_threads from server.toml.
    if (m_opts.simWorkers >= 0)
        cfg.world.simWorkerThreads = static_cast<uint32_t>(m_opts.simWorkers);
    // --flight-size overrides [flight] size. The game client's embedded single-player server passes
    // --flight-size 1, so single-player always flies with a wingman without changing the shipped
    // dedicated-server default (0), which would otherwise move every load-test number.
    if (m_opts.flightSize >= 0)
        cfg.flight.size = static_cast<uint32_t>(m_opts.flightSize);

    // ---- Select + create the transport backend (now that [network].transport is known) ----
    // --transport <gns|enet> from the pre-pass overrides the config. Single-player LocalServer passes
    // --transport enet so the enet6 game client and this server match.
    if (!m_opts.transport.empty())
        cfg.network.transport = m_opts.transport;
    const TransportKind transportKind = parseTransportKind(cfg.network.transport, TransportKind::Gns);
    p.network = createNetwork(transportKind, log);
    net = p.network.get();
    net->setAllowInsecure(cfg.network.allowInsecure);
    net->setNagleTime(cfg.network.gnsNagleTimeUs);
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "transport: %s", networkBackendVersion(transportKind));
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // Resolve the mission to load at startup (#854): --mission wins, else the first [rotation] item.
    // Multi-item rotation *timing* (advancing to the next item over the running server) lands
    // incrementally; the parse -> load -> sim-setup wiring is the deliverable here. The actual load
    // happens below once the entity registry + weather controller exist (see "load startup mission").
    missionToLoad = m_opts.mission;
    if (missionToLoad.empty() && !cfg.rotation.items.empty())
        missionToLoad = cfg.rotation.items.front();
    // A rotation item may pair a mission with a game mode: "mission@builtin:tdm" (#521). Split the mode
    // ref off so it never reaches the mission loader; an item without '@' uses the [match] mode default.
    modeRefToLoad = cfg.match.mode;
    {
        auto [missionRef, modeRef] = fl::splitRotationItem(missionToLoad);
        missionToLoad = missionRef;
        if (!modeRef.empty())
            modeRefToLoad = modeRef;
    }
    if (!cfg.rotation.items.empty() && m_opts.mission.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "rotation: %zu item(s), order=%s (loading first: %s)",
                      cfg.rotation.items.size(), cfg.rotation.order.c_str(), cfg.rotation.items.front().c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    if (!cfg.mods.stack.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "mod stack: %zu mod ID(s) configured (explicit ordering not yet active)",
                      cfg.mods.stack.size());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    return true;
}

bool ServerRuntime::Impl::initNet() {
    // The members this phase touches, bound by name so the moved code reads unchanged --
    // and so its `[&name]` lambda captures still compile: you cannot capture a member.
    [[maybe_unused]] auto& beacon = m_beacon;
    [[maybe_unused]] auto& cfg = m_cfg;
    [[maybe_unused]] auto& httpClient = m_httpClient;
    [[maybe_unused]] auto& listeningMsg = m_listeningMsg;
    [[maybe_unused]] auto& lobbyReg = m_lobbyReg;
    [[maybe_unused]] auto& log = m_log;
    [[maybe_unused]] auto& net = m_net;
    [[maybe_unused]] auto& primeSpawnHeight = m_primeSpawnHeight;
    [[maybe_unused]] auto& queryResponder = m_queryResponder;
    // ---- Init network ----
    if (!net->init()) {
        log->log(LogLevel::Error, __FILE__, __LINE__, "network init failed");
        m_exitCode = 1;
        return false;
    }

    if (!net->bind(cfg.server.bindAddress.c_str(), cfg.server.port, cfg.server.maxPeers)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bind failed: %s", net->getLastError() ? net->getLastError() : "unknown");
        log->log(LogLevel::Error, __FILE__, __LINE__, buf);
        net->shutdown();
        m_exitCode = 1;
        return false;
    }

    // "listening on" is printed after startup is complete (after primeSpawnHeight and all
    // pre-loop setup), so LocalServer::start() and CI smoke tests that wait for this line
    // only proceed once ENet is actually being serviced by the game loop.
    // Stored here for use after pre-loop setup completes (see below).
    // listeningMsg is a member (see Impl)
    std::snprintf(listeningMsg, sizeof(listeningMsg), "listening on %s:%u (max %d peers) name=\"%s\"",
                  cfg.server.bindAddress.c_str(), cfg.server.port, cfg.server.maxPeers, cfg.server.name.c_str());

    if (cfg.security.incomingBandwidthBps || cfg.security.outgoingBandwidthBps) {
        net->setBandwidthLimit(cfg.security.incomingBandwidthBps, cfg.security.outgoingBandwidthBps);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "bandwidth cap: in=%u B/s out=%u B/s", cfg.security.incomingBandwidthBps,
                      cfg.security.outgoingBandwidthBps);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    net->setPreHandshakeRateLimit(cfg.security.preHandshakeRateLimitCount, cfg.security.preHandshakeWindowMs);

    // ---- LAN discovery beacon ----
    uint8_t discoveryGameModeFlags = 0;
    for (const auto& m : cfg.server.gameModes) {
        if (m == "campaign")
            discoveryGameModeFlags |= fl::kGameModeCampaign;
        else if (m == "mission")
            discoveryGameModeFlags |= fl::kGameModeMission;
        else if (m == "sandbox")
            discoveryGameModeFlags |= fl::kGameModeSandbox;
    }
    if (!cfg.server.password.empty())
        discoveryGameModeFlags |= fl::kGameModePassworded; // #998 — browsers show a lock icon
    // Server info query responder (#997): a dedicated UDP port that answers A2S-style queries for the
    // server browser's ping/details column. Auto port = game port + 1.
    const uint16_t queryPort = cfg.discovery.queryPort != 0 ? static_cast<uint16_t>(cfg.discovery.queryPort)
                                                            : static_cast<uint16_t>(cfg.server.port + 1);
    // queryResponder is a member (see Impl)
    if (cfg.discovery.queryEnabled && !m_opts.noDiscovery) {
        queryResponder = std::make_unique<fl::ServerQueryResponder>(queryPort, *log);
        if (!queryResponder->start()) {
            log->log(LogLevel::Warn, __FILE__, __LINE__, "server query responder: bind failed; queries disabled");
            queryResponder.reset();
        } else {
            fl::ServerQueryResponder::StaticInfo si;
            si.name = cfg.server.name;
            si.gamePort = cfg.server.port;
            si.buildVersion = FL_VERSION_STRING; // #1074
            si.maxPlayers = static_cast<uint8_t>(cfg.server.maxPeers > 255 ? 255 : cfg.server.maxPeers);
            si.gameModeFlags = discoveryGameModeFlags;
            queryResponder->setStaticInfo(std::move(si));
            log->log(LogLevel::Info, __FILE__, __LINE__, "server query responder started");
        }
    }

    // beacon is a member (see Impl)
    if (cfg.discovery.enabled && !m_opts.noDiscovery) {
        DiscoveryBeacon::Config dcfg;
        dcfg.name = cfg.server.name;
        dcfg.gamePort = cfg.server.port;       // advertised as MsgLanBeacon::gamePort — where clients connect
        dcfg.buildVersion = FL_VERSION_STRING; // #1074: the browser shows it without connecting
        // Broadcast to the dedicated discovery port (#1071), never to the game port. The old alias is
        // why a client could not run its browser while a dedicated server held the game port.
        dcfg.discoveryPort = fl::kDiscoveryPort;
        dcfg.maxPlayers = static_cast<uint8_t>(cfg.server.maxPeers > 255 ? 255 : cfg.server.maxPeers);
        dcfg.gameModeFlags = discoveryGameModeFlags;
        dcfg.queryPort = queryResponder ? queryPort : 0; // #997: advertise the query port to browsers
        dcfg.intervalMs = cfg.discovery.intervalMs;
        dcfg.broadcastAddr = "255.255.255.255";
        beacon = std::make_unique<DiscoveryBeacon>(dcfg, *log);
        if (!beacon->isOpen()) {
            log->log(LogLevel::Warn, __FILE__, __LINE__, "LAN discovery beacon: no sockets opened; discovery disabled");
            beacon.reset();
        } else {
            log->log(LogLevel::Info, __FILE__, __LINE__, "LAN discovery beacon started");
        }
    }

    // Lobby registration (#143): register with a lobby over HTTP so the entry appears in players'
    // server browsers. Requires libcurl (the HTTP backend); a lean build logs + disables it. Serviced +
    // ticked in the main loop, deregistered on shutdown. fl-server links platform-http (allowed: the
    // module-boundary rules ban engine-* from a backend and fl-server from CLIENT backends only).
    // httpClient is a member (see Impl)
    // lobbyReg is a member (see Impl)
    if (cfg.lobby.registerServer) {
        httpClient = fl::createHttpClient(log);
        if (httpClient && httpClient->init()) {
            lobbyReg = std::make_unique<LobbyRegistration>(*httpClient, *log);
            LobbyRegistrationConfig lc;
            lc.lobbyUrl = cfg.lobby.url;
            lc.name = cfg.server.name;
            lc.gamePort = cfg.server.port;
            lc.maxPlayers = cfg.server.maxPeers;
            lc.mode = cfg.match.mode;
            lc.visibilityPublic = (cfg.lobby.visibility == "public");
            lobbyReg->configure(lc);
            if (lobbyReg->enabled()) {
                log->log(LogLevel::Info, __FILE__, __LINE__, "lobby registration active");
            } else {
                // Two things disable it, and the message used to name only one. With [lobby] url now
                // defaulting to empty (#1072), "register = true and no url" is the LIKELIER case, and
                // an operator told "visibility=private" when they set public would look in the wrong
                // place. Same defect class as the flag this issue removed.
                log->log(LogLevel::Info, __FILE__, __LINE__,
                         cfg.lobby.url.empty() ? "lobby registration requested but [lobby] url is empty; disabled "
                                                 "(set it to a lobby's base URL -- the reference service is issue #999)"
                                               : "lobby registration disabled (visibility=private)");
            }
        } else {
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "lobby registration requested but no HTTP backend (libcurl absent); disabled");
            httpClient.reset();
        }
    }
    return true;
}

bool ServerRuntime::Impl::initContent() {
    // The members this phase touches, bound by name so the moved code reads unchanged --
    // and so its `[&name]` lambda captures still compile: you cannot capture a member.
    [[maybe_unused]] auto& airportRegistry = m_airportRegistry;
    [[maybe_unused]] auto& assetsRoot = m_assetsRoot;
    [[maybe_unused]] auto& cachedSpawns = m_cachedSpawns;
    [[maybe_unused]] auto& cfg = m_cfg;
    [[maybe_unused]] auto& contentIndex = m_contentIndex;
    [[maybe_unused]] auto& entityRegistry = m_entityRegistry;
    [[maybe_unused]] auto& gameMode = m_gameMode;
    [[maybe_unused]] auto& log = m_log;
    [[maybe_unused]] auto& modeRefToLoad = m_modeRefToLoad;
    [[maybe_unused]] auto& nearSideSurface = m_nearSideSurface;
    [[maybe_unused]] auto& p = m_p;
    [[maybe_unused]] auto& planetR = m_planetR;
    [[maybe_unused]] auto& primeSpawnHeight = m_primeSpawnHeight;
    [[maybe_unused]] auto& primeSpawnHeightUntil = m_primeSpawnHeightUntil;
    [[maybe_unused]] auto& userDataRoot = m_userDataRoot;
    [[maybe_unused]] auto& weaponRegistry = m_weaponRegistry;
    // ---- Content system and headless terrain ----
    // Content root resolution mirrors the client (#831) so a single-player pair agrees on where
    // mods/ lives: --assets <dir> > FL_ASSETS_ROOT > the current working directory. LocalServer
    // forwards the client's resolved root via --assets, so the two never disagree.
    assetsRoot = fs::current_path();
    if (!m_opts.assets.empty())
        assetsRoot = fs::path(m_opts.assets);
    else if (const char* ev = std::getenv("FL_ASSETS_ROOT"); ev && *ev)
        assetsRoot = fs::path(ev);
    userDataRoot = fs::current_path();

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
            m_exitCode = 1;
            return false;
        }
        p.asyncFilesystem = std::move(asyncFs);
    }

    m_modLoader = std::make_unique<ModLoader>(*p.filesystem, *log, assetsRoot.string());
    [[maybe_unused]] auto& modLoader = *m_modLoader;
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

    m_assets = std::make_unique<AssetManager>(std::move(packs), *log);
    [[maybe_unused]] auto& assets = *m_assets;
    assets.initialize(nullptr); // headless — window is null; NeedsConfiguration packs dropped

    // Resolve the active game mode (#521): a builtin id, a pack modes/ asset, or the free-flight
    // fallback. The MatchController (#523) consumes it; landing it here proves the resolution path and
    // surfaces a bad [match] mode / rotation @mode at startup. Serial-equivalent: pure config read.
    gameMode = fl::resolveGameMode(modeRefToLoad, &assets, *log);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "game mode: %s (%s)%s", gameMode.id.c_str(),
                      gameMode.name.empty() ? gameMode.id.c_str() : gameMode.name.c_str(),
                      gameMode.useMissionSides ? " [mission sides]" : "");
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    (void)gameMode; // consumed by the MatchController in #523

    m_terrainStreamer =
        std::make_unique<fl::TerrainStreamer>(fl::builtinWorldTerrainManifest(), assets, *p.asyncFilesystem, nullptr);
    [[maybe_unused]] auto& terrainStreamer = *m_terrainStreamer;
    log->log(LogLevel::Info, __FILE__, __LINE__, "terrain: headless streamer initialized");

    // Apply the configured planet radius BEFORE the first update(): tiles bake curvature and
    // procedural elevations at generation time, so streaming first would generate Earth-radius
    // terrain (and prime wrong spawn elevations) on a non-Earth planet.
    terrainStreamer.setPlanetRadius(cfg.world.planetRadiusM);

    // Kick off terrain streaming at the origin. A single update() is not enough to guarantee the
    // spawn point's covering tile chain is Ready (procedural loads are rate-limited per update;
    // content-pack tiles load asynchronously), so spawn elevations are primed below via
    // primeSpawnHeight() before they are queried.
    // The home, not the origin (#1211) — priming the pole would stream tiles nobody is standing on.
    // planetR is not read from cfg yet at this point, so use the streamer's own configured radius.
    {
        double hx = 0.0, hy = 0.0, hz = 0.0;
        fl::geodeticToWorld(fl::sandboxHome(), hx, hy, hz, cfg.world.planetRadiusM);
        terrainStreamer.update(glm::dvec3(hx, hy, hz));
    }

    // ---- Content index (#810) ----
    // Def cross-references (an entity's `sensors`, a hardpoint's `allowed`/`default`) are namespaced
    // IDS, while AssetManager resolves assets by FILENAME STEM. This index reconciles the two, once,
    // and is the only place they meet: without it, `sensors = ["fl-base:apq159"]` builds the path
    // "sensors/fl-base:apq159.toml" and every aircraft in the pack silently flies with no radar.
    // contentIndex is a member (see Impl)
    {
        static constexpr fl::AssetType kIndexedTypes[] = {fl::AssetType::EntityDef, fl::AssetType::SensorDef};
        contentIndex.build(assets, kIndexedTypes, *log);
    }

    // ---- Weapons (#812) ----
    // Registered BEFORE entity defs, so an entity's hardpoints have weapons to resolve against.
    // Keyed by id, so that resolution never touches the filesystem.
    // weaponRegistry is a member (see Impl)
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
    // entityRegistry is a member (see Impl)
    m_entityManager = std::make_unique<fl::EntityManager>(*log, entityRegistry);
    [[maybe_unused]] auto& entityManager = *m_entityManager;

    // World object ceiling (#1049). Applied HERE, before anything spawns, so the mission load, the
    // load-test pre-spawn and every runtime spawn are bounded by it. The key was parsed and
    // range-checked since Phase 2 but never reached the pool — a resource control that quietly did
    // nothing, which is worse than an absent one because operators rely on it.
    //
    // The player reserve is max_peers. The server never admits more pilots than that, so holding back
    // one airframe each is exactly enough to guarantee a joining or respawning human is never locked
    // out by a world full of projectiles and AI — the failure mode a flat cap has by construction.
    // It is derived rather than configured because there is only one correct value for it; EntityPool
    // clamps it to half the cap so a big max_peers against a small cap cannot starve the world.
    if (cfg.world.entitySoftCap > 0) {
        const auto cap = static_cast<uint32_t>(cfg.world.entitySoftCap);
        const auto wanted = static_cast<uint32_t>(cfg.server.maxPeers < 0 ? 0 : cfg.server.maxPeers);
        entityManager.setSoftCap(cap, wanted);
        const uint32_t reserve = entityManager.playerReserve();
        char buf[224];
        std::snprintf(buf, sizeof(buf), "world: entity soft cap %u (player reserve %u, world spawns capped at %u)", cap,
                      reserve, cap - reserve);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        if (reserve < wanted) {
            std::snprintf(buf, sizeof(buf),
                          "world.entity_soft_cap (%u) is small relative to max_peers (%d): the player reserve was "
                          "clamped from %u to %u, so a full lobby may not find airframes",
                          cap, cfg.server.maxPeers, wanted, reserve);
            log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        }
    }

    // The debug entity ships ARMED (#440) — the shared builder keeps server and client identical.
    entityRegistry.registerType(fl::builtinDebugEntityDef());
    // The builtin multi-crew bomber (#966/#977): a pilot + a bot tail-gunner turret, so the whole
    // crew/turret fire path is provable zero-pack (the crewed counterpart to the debug entity).
    entityRegistry.registerType(fl::builtinBomberDef());
    // The sensor-carrying fighter (#1089): the debug entity plus an eyeball and an intercept radar.
    // The scale gate flies this so contact tables populate and datalink fusion is actually measured.
    entityRegistry.registerType(fl::builtinSensorFighterDef());
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
    // Spawn config coordinates are planar (x, z), and they are METRES EAST AND NORTH OF THE SANDBOX
    // HOME (#1211) — the same authoring frame a mission's `anchor:` gives content. They used to be
    // raw world x/z mapped onto the sphere's NEAR side, which is a real place only within a few tens
    // of km of the world origin: the near-side solve (y = -R + sqrt((R+h)^2 - x^2 - z^2)) has no
    // solution once x^2 + z^2 exceeds R^2, and the home is 5,977 km from the origin. Every existing
    // number keeps its meaning, because the home is exactly what the origin used to stand in for.
    planetR = cfg.world.planetRadiusM;
    nearSideSurface = [&](double x, double z, double agl) -> glm::dvec3 {
        // The column: the home offset east/north, at sea level, to sample the terrain under.
        double cx = 0.0, cy = 0.0, cz = 0.0;
        fl::localOffsetToWorld(fl::sandboxHome(), x, z, 0.0, cx, cy, cz, planetR);
        const double h = terrainStreamer.heightAt(glm::dvec3{cx, cy, cz}); // radial terrain elevation
        double sx = 0.0, sy = 0.0, sz = 0.0;
        fl::localOffsetToWorld(fl::sandboxHome(), x, z, h + agl, sx, sy, sz, planetR);
        return glm::dvec3{sx, sy, sz};
    };
    // Pump terrain streaming until the covering tile chain above the (x, z) column is Ready to
    // heightReadyAt depth, so the spawn elevation is spawn-accurate rather than a coarse or datum
    // placeholder. Bounded by a wall-clock deadline so a missing/stuck tile can never hang startup.
    // Without this the entity spawns too low and the per-tick floor query snaps it up to the terrain
    // surface once it streams in, which the client sees as the camera jumping after load-in.
    // Returns whether the column ended up spawn-accurate. Split out so the load spawn can prime a
    // whole spread of points under ONE shared budget and report the shortfall as a count (#1137),
    // rather than a per-point 5 s timeout and a warning per point.
    primeSpawnHeightUntil = [&](double x, double z, std::chrono::steady_clock::time_point deadline) -> bool {
        using namespace std::chrono;
        // Pump at the near-side surface estimate, not world-Y 0: far from the origin the near side
        // of the sphere sits well below y=0, and SSE refinement only reaches heightReadyAt depth
        // when the pumped position is close to the surface. The estimate converges as tiles stream.
        while (!terrainStreamer.heightReadyAt(nearSideSurface(x, z, 0.0)) && steady_clock::now() < deadline) {
            terrainStreamer.update(nearSideSurface(x, z, 0.0));
            p.asyncFilesystem->service();
            if (!terrainStreamer.heightReadyAt(nearSideSurface(x, z, 0.0)))
                std::this_thread::sleep_for(milliseconds(2));
        }
        return terrainStreamer.heightReadyAt(nearSideSurface(x, z, 0.0));
    };
    primeSpawnHeight = [&](double x, double z) {
        using namespace std::chrono;
        if (!primeSpawnHeightUntil(x, z, steady_clock::now() + seconds(5)))
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "terrain: spawn-point chunk not ready within timeout; spawning at AGL only");
    };

    // Entity meshes are authored with their origin at the ground-contact point (see
    // gen_builtin_glb.py), so the physics floor can clamp the origin straight to the terrain and
    // the mesh sits ON the ground — no per-mesh clearance offset needed.
    // cachedSpawns is a member (see Impl)
    {
        const double agl = cfg.spawn.aglOffset;
        if (cfg.spawn.points.empty()) {
            // Default spawn: the sandbox home itself — where the builtin airfield stands (#1211).
            primeSpawnHeight(0.0, 0.0);
            const glm::dvec3 s = nearSideSurface(0.0, 0.0, agl);
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
    // airportRegistry is a member (see Impl)
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
    return true;
}

bool ServerRuntime::Impl::initWorld() {
    // The members this phase touches, bound by name so the moved code reads unchanged --
    // and so its `[&name]` lambda captures still compile: you cannot capture a member.
    [[maybe_unused]] auto& aiScriptCache = m_aiScriptCache;
    [[maybe_unused]] auto& airportRegistry = m_airportRegistry;
    [[maybe_unused]] auto& assets = *m_assets;
    [[maybe_unused]] auto& cachedSpawns = m_cachedSpawns;
    [[maybe_unused]] auto& cfg = m_cfg;
    [[maybe_unused]] auto& configPath = m_configPath;
    [[maybe_unused]] auto& contentIndex = m_contentIndex;
    [[maybe_unused]] auto& entityManager = *m_entityManager;
    [[maybe_unused]] auto& entityRegistry = m_entityRegistry;
    [[maybe_unused]] auto& fmCache = m_fmCache;
    [[maybe_unused]] auto& log = m_log;
    [[maybe_unused]] auto& nearSideSurface = m_nearSideSurface;
    [[maybe_unused]] auto& net = m_net;
    [[maybe_unused]] auto& p = m_p;
    [[maybe_unused]] auto& primeSpawnHeight = m_primeSpawnHeight;
    [[maybe_unused]] auto& resolveAiScaling = m_resolveAiScaling;
    [[maybe_unused]] auto& terrainStreamer = *m_terrainStreamer;
    [[maybe_unused]] auto& weaponRegistry = m_weaponRegistry;
    [[maybe_unused]] auto& wingmanParams = m_wingmanParams;
    [[maybe_unused]] auto& wparams = m_wparams;
    // ---- WorldBroadcaster wires the sim loop to ENet ----
    // wparams is a member (see Impl)
    wparams.timeScaleRatio = static_cast<float>(cfg.world.timeScale);
    m_weatherController = std::make_unique<fl::WeatherController>(wparams);
    [[maybe_unused]] auto& weatherController = *m_weatherController;
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
    // The host seams, built here and FROZEN at construction (#1082, D12). Every field is
    // optional and null still means "that feature is off" -- what the 19 deleted setters meant.
    // A hook that reaches back into the broadcaster captures `this`: valid before the member
    // exists, which is why this lands after ServerRuntime (#1084) rather than before it.
    fl::WorldQueries queries;
    fl::WorldBroadcasterHooks hooks;

    // The ENet admin frontend (#1079/#1082). Built here rather than in initAdmin because the
    // broadcaster takes it at construction; an empty operator password leaves it null, which is what
    // turns MsgAdminCommand handling OFF -- exactly as an unset dispatcher used to. Its shell tap and
    // its registry entry are still wired in initAdmin, where the shell exists.
    if (!cfg.security.operatorPassword.empty()) {
        fl::AdminChannel::Config enetCfg;
        enetCfg.name = "enet";
        enetCfg.maxAuthFailures = cfg.security.adminAuthMaxFailures;
        enetCfg.lockoutSeconds = cfg.security.adminAuthLockoutSeconds;
        m_enetChannel = std::make_unique<fl::AdminChannel>(
            [this](std::string_view cmd, const fl::CommandIssuer& issuer) {
                // Permission-check against the issuer's granted capabilities (#946). The registry is
                // still empty here and is filled in initAdmin; dispatch only happens once a peer
                // sends a command, long after that.
                return m_adminRegistry.dispatch(cmd, issuer);
            },
            std::move(enetCfg), fl::SystemClock::instance());
        hooks.comms.adminChannel = m_enetChannel.get();
    }

    // The four seams the mission and the recorder decide later. Each forward reproduces the UNSET
    // behaviour when its member is empty, which is what lets them be installed unconditionally:
    hooks.admission.missionSlotBinder = [this](const std::string& id, fl::EntityId eid) {
        if (m_missionSlotBind) // unset = notify nobody
            m_missionSlotBind(id, eid);
    };
    queries.teamAssigner = [this](uint32_t peerId) -> std::optional<uint16_t> {
        // kNoFaction is the legacy path -- the mission slot's faction, else the configured player
        // faction -- which is precisely what the broadcaster did with no assigner installed. nullopt
        // still means "every team is full, refuse the connection".
        return m_teamAssign ? m_teamAssign(peerId) : std::optional<uint16_t>{fl::WorldBroadcaster::kNoFaction};
    };
    hooks.match.teamSwitchGuard = [this](uint32_t peerId, uint16_t target) {
        return !m_teamSwitchAllowed || m_teamSwitchAllowed(peerId, target); // unset = all switches allowed
    };
    hooks.match.shutdown = []() { g_quit = 1; };
    // The replay tap stays CONDITIONAL: a null sink means the tap stream is not built at all, so
    // installing one on a server with replay disabled would cost every tick for nothing. Whether
    // recording is on is known here; the recorder itself is not built until initSystems.
    if (cfg.replay.enabled) {
        hooks.snapshot.replaySink = [this](const fl::ReplayTickRecords& rec) {
            if (m_replayTap)
                m_replayTap(rec);
        };
        hooks.snapshot.replayKeyframeIntervalTicks = cfg.replay.keyframeIntervalTicks;
    }

    // Per-entity terrain height query: sim thread calls heightAt() (thread-safe via shared_mutex).
    // The entity origin is the mesh's ground-contact point, so the floor clamps it directly to the
    // terrain — the mesh then rests ON the ground.
    queries.groundElevation = [&terrainStreamer](glm::dvec3 pos) {
        return static_cast<float>(terrainStreamer.heightAt(pos));
    };

    // Per-entity ground surface (#487): the runway-surface override + land cover, so the rollout
    // differs by surface. surfaceTypeAt is thread-safe (shared_mutex); the client mirrors it.
    queries.groundSurface = [&terrainStreamer](glm::dvec3 pos) { return terrainStreamer.surfaceTypeAt(pos); };

    // Base operations (#55): the crew chief services an aircraft shut down within a few km of an
    // airport (or on a carrier deck, which WorldBroadcaster checks itself). AirportRegistry is
    // load-once/lock-free, safe to query from the sim thread.
    queries.baseProximity = [&airportRegistry](glm::dvec3 pos) {
        constexpr double kBaseServiceRangeM = 5000.0;
        return airportRegistry.nearestTo(pos.x, pos.z, kBaseServiceRangeM) != nullptr;
    };

    // Resolve EntityDef::flightModelAsset -> parsed FlightModelData on the spawn path. The
    // load-and-parse step is shared with the client resolver (#1232) so malformed content is a
    // logged null — never an exception through the sim spawn path — and caches the result by id
    // (sim-thread-only access). Empty/unknown/malformed ids fall back to the builtin model in
    // WorldBroadcaster. Captures the cache pointer by value — see m_fmCache's declaration (#1178).
    queries.flightModel = [&assets, fmCache, log](const std::string& id) -> std::shared_ptr<const fl::FlightModelData> {
        // The compiled-in carrier's vessel model (#38): a "builtin:" name never touches the
        // filesystem, same rule as every other builtin asset.
        if (id == "builtin:carrier-vessel")
            return fl::BuiltinCarrierVesselModel::get();
        if (auto it = fmCache->find(id); it != fmCache->end())
            return it->second;
        auto res = fl::loadAndParseFlightModel(assets, id.c_str());
        if (!res.model)
            log->log(fl::LogLevel::Error, __FILE__, __LINE__,
                     (res.error + " -- entities of this type will fly the builtin model").c_str());
        (*fmCache)[id] = res.model; // cache misses too, so a bad id isn't re-parsed every connect
        return res.model;
    };

    // What an entity's DEFAULT loadout costs it in mass and drag (#812). Same injection shape as the
    // resolvers above -- the summation lives in engine-weapon, and engine-net must not link it.
    // Unset would mean every aircraft flies clean, which is exactly the bug this closes.
    queries.payload = [&weaponRegistry, log](const fl::EntityDef& def) -> fl::PayloadEffect {
        return fl::defaultPayload(def, weaponRegistry, *log);
    };

    // Build a crewed aircraft's non-fly seat bots (#971). engine-net does not link engine-ai
    // (cmake/layering.cmake), so the concrete seat bots — the turret gunner — reach the broadcaster
    // through this std::function seam, exactly like the flight-order / target-designator hooks below.
    queries.seatControllerFactory =
        [&entityManager](const fl::SeatDef& seat, uint8_t seatIdx,
                         const fl::SeatBotContext& ctx) -> std::unique_ptr<fl::ISeatController> {
        return fl::ai::makeSeatController(seat, seatIdx, entityManager, ctx.skillMin, ctx.skillMax, ctx.missionSeed);
    };

    // Resolve EntityDef::sensorIds -> parsed SensorDef on the spawn path (#685). A sensor reference
    // is an ID, not an asset name, so it goes through ContentIndex (#810) -- see makeSensorDefResolver.
    queries.sensorDefs = fl::makeSensorDefResolver(assets, contentIndex, *log);

    hooks.comms.chatModeration = [log](uint32_t peerId, uint8_t channel, std::string_view text) {
        char buf[320];
        std::snprintf(buf, sizeof(buf), "[chat] peer %u ch%u: %.*s", peerId, static_cast<unsigned>(channel),
                      static_cast<int>(text.size()), text.data());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        return true; // allow
    };

    // Only when a flight is configured: an unset spawner means "the peer flies alone", and a spawner
    // that builds an empty formation is not the same thing.
    if (cfg.flight.size > 0) {
        const std::string flightEntityType = cfg.flight.entityType;
        const uint32_t flightSize = cfg.flight.size;
        queries.flightSpawner = [this, &entityManager, flightEntityType, flightSize,
                                 wingmanParams](uint32_t peerId, fl::EntityId leadEntity) -> fl::FormationId {
            const fl::EntityState* lead = entityManager.get(leadEntity);
            if (!lead)
                return fl::kNoFormation;

            // The player is the anchor AND the commander of their own flight — the common case, but
            // only a special case of the general model (an AWACS commands a flight it is not in).
            const fl::FormationId fid =
                m_broadcaster->formations().create("Viper", leadEntity, peerId, fl::kNoFormation);
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
                m_broadcaster->registerController(id, fl::ai::makeWingmanController(entityManager, leadEntity,
                                                                                    fl::ai::WingmanCommand::Rejoin,
                                                                                    fl::EntityId{}, wp));

                fl::FormationMember m{};
                m.id = id;
                m.peerId = fl::kNoPeer; // AI: the server owns this aircraft and may retask it
                m.slotIndex = slot;
                m_broadcaster->formations().addMember(fid, m);
            }
            return fid;
        };
    }

    // Retask one AI member. The formation supplies the anchor (who to form on) and the member its
    // slot, so an order never has to be told where the aircraft belongs.
    hooks.comms.flightOrders = [this, &entityManager, wingmanParams](const fl::Formation& formation,
                                                                     const fl::FormationMember& member, uint8_t command,
                                                                     fl::EntityId designatedTarget) -> bool {
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
        m_broadcaster->registerController(member.id, std::move(ctrl)); // REPLACES the existing controller
        return true;
    };

    {
        const auto designateRangeM = static_cast<float>(cfg.flight.designateRangeM);
        const auto halfAngleRad =
            static_cast<float>(cfg.flight.designateHalfAngleDeg * std::numbers::pi_v<double> / 180.0);
        queries.targetDesignator = [this, &entityManager, designateRangeM, halfAngleRad](
                                       const fl::EntityState& commander, const float viewAxis[3]) -> fl::EntityId {
            if (const fl::sensor::ContactTable* contacts = m_broadcaster->contactsFor(commander.id.index)) {
                // The honest path: a lead cannot order an attack on something it has not seen. If it
                // is pointing at empty sky, this returns an invalid id and the order is REFUSED
                // ("Two, no joy") rather than quietly retargeted — which is the whole principle.
                return fl::ai::designateFromContacts(commander, viewAxis, contacts, designateRangeM, halfAngleRad);
            }
            return fl::ai::designateBoresightTarget(entityManager, commander, viewAxis, designateRangeM, halfAngleRad,
                                                    &m_broadcaster->spatialIndex());
        };
    }

    m_broadcaster = std::make_unique<fl::WorldBroadcaster>(entityManager, entityRegistry, *net, *log,
                                                           &weatherController, std::move(queries), std::move(hooks));
    [[maybe_unused]] auto& broadcaster = *m_broadcaster;
    broadcaster.setParachuteType("builtin:parachute"); // spawn a chute on pilot ejection (#672)
    broadcaster.setAiAutoEject(true);                  // AI pilots punch out when critically hit (#672)
    fl::WorldBroadcasterConfig wbConfig;
    wbConfig.connectRateLimit = cfg.security.connectRateLimitCount;
    wbConfig.connectRateWindowS = cfg.security.connectRateLimitWindowS;
    wbConfig.floodMultiplier = cfg.security.packetFloodMultiplier;
    wbConfig.maxConnectionsPerIp = cfg.security.maxConnectionsPerIp;
    wbConfig.motd = cfg.server.motd;
    wbConfig.motdDisplaySeconds = cfg.server.motdDisplayS;
    wbConfig.operatorPassword = cfg.security.operatorPassword;
    wbConfig.playerEntityType = cfg.world.playerEntityType; // pilot spawn default when client requests none (#834)
    wbConfig.allowObservers = cfg.world.allowObservers;     // #857
    wbConfig.requiredPacks.clear();                         // #872: parse "id" / "id@version" specs into RequiredPack
    for (const auto& spec : cfg.mods.requiredPacks)
        wbConfig.requiredPacks.push_back(fl::parseRequiredPackSpec(spec));
    wbConfig.requiredPackPolicy =
        fl::parseRequiredPackPolicy(cfg.mods.requiredPackPolicy).value_or(fl::RequiredPackPolicy::Warn);
    wbConfig.idleTimeoutS = cfg.security.idleTimeoutS;
    wbConfig.drawDistanceKm = static_cast<float>(cfg.world.drawDistanceKm);
    wbConfig.snapshotBudgetBytes = cfg.world.snapshotBudgetBytes;
    wbConfig.compressSnapshots = cfg.network.compressSnapshots;
    wbConfig.jitterBufferMaxDepth = cfg.world.jitterBufferDepth;
    wbConfig.jitterAdaptWindow = cfg.world.jitterAdaptWindow;
    wbConfig.jitterHysteresis = cfg.world.jitterHysteresis;
    wbConfig.jitterMultiplier = cfg.world.jitterMultiplier;
    wbConfig.congestion =
        fl::makeCongestionParams(cfg.world.congestionEnabled, cfg.world.congestionMinSendHz,
                                 cfg.world.congestionLossThreshold, cfg.world.congestionBudgetFloorBytes);
    wbConfig.governor = fl::makeTickGovernorParams(cfg.world.overrunGovernorEnabled, cfg.world.overrunHighWatermark,
                                                   cfg.world.overrunLowWatermark, cfg.world.overrunMinSnapshotHz,
                                                   cfg.world.overrunMaxAiStride, cfg.world.overrunBudgetFloorBytes,
                                                   cfg.world.overrunMinInterestFraction);
    wbConfig.gameplay = fl::DamageRules{cfg.gameplay.friendlyFire, cfg.gameplay.crashDamage};
    broadcaster.applyConfig(wbConfig);
    broadcaster.setJoinPassword(cfg.server.password); // #998: [server] password gates joins (empty = open)
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
        const auto R_m = static_cast<float>(cfg.world.planetRadiusM);
        s_gravity = fl::CentralGravityField(R_m);
        broadcaster.setGravityField(s_gravity, R_m / 1000.f);
    }
    // Earth-fixed rotating world frame: Coriolis + centrifugal on every integrator (#482).
    broadcaster.setEarthRotationRate(cfg.world.earthRotation ? fl::kEarthRotationRate : 0.0);
    broadcaster.setSensorCheckHz(static_cast<float>(cfg.world.sensorCheckHz));

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
    m_difficultyTable =
        std::make_unique<fl::DifficultyMultipliers>(fl::DifficultyMultipliers::load(assets, *p.filesystem, *log));
    [[maybe_unused]] auto& difficultyTable = *m_difficultyTable;
    resolveAiScaling = [&difficultyTable](const std::string& name) -> fl::AiScaling {
        fl::DifficultyPreset preset = fl::DifficultyPreset::Pilot;
        if (name == "cadet")
            preset = fl::DifficultyPreset::Cadet;
        else if (name == "ace")
            preset = fl::DifficultyPreset::Ace;
        fl::DifficultySettings ds{};
        difficultyTable.applyPreset(preset, ds);
        return ds.ai;
    };
    broadcaster.setAiScaling(resolveAiScaling(cfg.ai.difficulty));
    {
        const fl::AiScaling scaling = resolveAiScaling(cfg.ai.difficulty);
        char buf[192];
        std::snprintf(buf, sizeof(buf), "ai difficulty \"%s\": radar range x%.2f, reaction %.2f s",
                      cfg.ai.difficulty.c_str(), static_cast<double>(scaling.radarSensorRange),
                      static_cast<double>(scaling.reactionTimeS));
        log->log(fl::LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // ---- Formations and the scripted wingman (#610) ----------------------------------------------
    // engine-net does not link engine-ai (cmake/layering.cmake), so the three places an order needs
    // engine-ai — spawning a flight, building a controller, designating a target — are injected here
    // as std::functions. fl-server links both, so this is the seam where they meet.
    broadcaster.setPlayerFaction(cfg.world.playerFaction);
    broadcaster.setBuildVersion(FL_VERSION_STRING); // #1074: rides MsgHello so a peer knows the build
    broadcaster.setFlightCommandRateLimit(cfg.flight.commandRateLimitPerS);

    // World-mutating request limits (#1069): seat/team grants cost a despawn+respawn and a full
    // ConnectAck; heartbeats cost a reply. These bound how often a peer may ask for each.
    broadcaster.setSeatRequestRateLimit(cfg.security.seatRequestRateLimitPerS);
    broadcaster.setTeamSwitchCooldownSeconds(cfg.security.teamSwitchCooldownS);
    broadcaster.setHeartbeatRateLimit(cfg.security.heartbeatRateLimitPerS);

    // In-match text chat (#646). The moderation hook default logs an audit line and allows every message;
    // an operator replaces it with a filter. Rate limit + enable come from [chat].
    broadcaster.setChatEnabled(cfg.chat.enabled);
    broadcaster.setChatRateLimit(cfg.chat.rateLimitPerS);

    // In-game voice comms (Epic J, #532). The server relays opaque Opus frames by net membership and
    // never decodes one; everything here is routing + bandwidth policy. An empty [[voice.nets]] leaves
    // the compiled-in stack in place (setRadioNets falls back), so voice works with no configuration.
    broadcaster.setVoiceEnabled(cfg.voice.enabled);
    broadcaster.setVoiceFrameRateLimit(cfg.voice.frameRateLimit);
    if (!cfg.voice.nets.empty()) {
        RadioNetTable nets;
        for (const auto& n : cfg.voice.nets) {
            RadioNetDef def;
            def.id = n.id;
            def.name = n.name;
            if (!radioNetKindFromString(n.kind, def.kind)) {
                char buf[192];
                std::snprintf(buf, sizeof(buf), "[voice] net '%s': unknown kind '%s'; using 'team'", n.id.c_str(),
                              n.kind.c_str());
                log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
            }
            def.positional = n.positional;
            def.rangeM = static_cast<float>(n.rangeM);
            def.radioEffect = n.radioEffect;
            def.gain = static_cast<float>(n.gain);
            def.defaultNet = n.defaultNet;
            def.maxTalkers = static_cast<uint8_t>(n.maxTalkers); // #1090: concurrent-speaker cap
            if (nets.add(std::move(def)) == kInvalidRadioNet) {
                char buf[192];
                std::snprintf(buf, sizeof(buf), "[voice] net '%s' rejected (duplicate id or table full); skipped",
                              n.id.c_str());
                log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
            }
        }
        broadcaster.setRadioNets(std::move(nets));
    }
    if (cfg.voice.enabled) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "voice: %zu radio net(s), %d frames/s/peer", broadcaster.radioNets().size(),
                      cfg.voice.frameRateLimit);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // Spectator snapshot delay (#403): anti-ghosting for dead/observer peers (0 = off).
    broadcaster.setSpectateDelay(cfg.world.spectateDelayS);

    if (cfg.flight.size > 0 && cfg.world.playerFaction == 0) {
        // A flight whose threat logic can never fire is a silently broken feature, not a
        // configuration: areFactionsHostile gives a faction-0 entity no enemies at all.
        log->log(LogLevel::Warn, __FILE__, __LINE__,
                 "[flight] size > 0 but [world] player_faction = 0: players are NEUTRAL, so nothing is "
                 "hostile to them - the wingman's engage/cover orders will never trigger and "
                 "attack_my_target can never designate. Set player_faction = 1.");
    }

    // Formation geometry + behaviour tuning, shared by the spawner and the order handler so a
    // wingman spawned into a slot and a wingman ordered back to it agree on where the slot is.
    // wingmanParams is a member (see Impl)
    wingmanParams.formation.lateralM = static_cast<float>(cfg.flight.lateralM);
    wingmanParams.formation.aftM = static_cast<float>(cfg.flight.aftM);
    wingmanParams.formation.verticalM = static_cast<float>(cfg.flight.verticalM);
    wingmanParams.engageRangeM = static_cast<float>(cfg.flight.engageRangeM);
    wingmanParams.coverRangeM = static_cast<float>(cfg.flight.coverRangeM);

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
    // Wire pre-cached spawn positions. Must be called before gameLoop.start() (never mutated after).
    broadcaster.setSpawnPoints(std::move(cachedSpawns));
    // Seed the physics floor from the already-primed TerrainStreamer at origin.
    // Used by FlightIntegrator::step for ground collision. Updated each frame below.
    // Peer spawn positions are set separately via setSpawnPoints() above.
    primeSpawnHeight(0.0, 0.0); // no-op if already Ready (default-spawn path primed it above)
    // The reference ground elevation is sampled at the HOME, not the world origin (#1211): the
    // origin is the north pole, thousands of km from anything this server is simulating.
    broadcaster.setGroundElevation(static_cast<float>(terrainStreamer.heightAt(nearSideSurface(0.0, 0.0, 0.0))));

    // A read-only AI script cache, built before the mission load (so mission `ai: lua <name>` objects
    // can resolve their scripts) and reused later by the ENet admin `spawn --ai lua` path. Safe to read
    // from any thread — it is never mutated after construction, before gameLoop.start().
    // aiScriptCache is a member (see Impl)
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
    return true;
}

bool ServerRuntime::Impl::initMission() {
    // The members this phase touches, bound by name so the moved code reads unchanged --
    // and so its `[&name]` lambda captures still compile: you cannot capture a member.
    [[maybe_unused]] auto& adminRegistry = m_adminRegistry;
    [[maybe_unused]] auto& aiScriptCache = m_aiScriptCache;
    [[maybe_unused]] auto& assets = *m_assets;
    [[maybe_unused]] auto& botRoster = m_botRoster;
    [[maybe_unused]] auto& broadcaster = *m_broadcaster;
    [[maybe_unused]] auto& campaignMissionId = m_campaignMissionId;
    [[maybe_unused]] auto& campaignRunner = m_campaignRunner;
    [[maybe_unused]] auto& campaignSavePath = m_campaignSavePath;
    [[maybe_unused]] auto& campaignYaml = m_campaignYaml;
    [[maybe_unused]] auto& cfg = m_cfg;
    [[maybe_unused]] auto& entityManager = *m_entityManager;
    [[maybe_unused]] auto& entityRegistry = m_entityRegistry;
    [[maybe_unused]] auto& gameMode = m_gameMode;
    [[maybe_unused]] auto& loadedMissionName = m_loadedMissionName;
    [[maybe_unused]] auto& loadedMissionSpawned = m_loadedMissionSpawned;
    [[maybe_unused]] auto& log = m_log;
    [[maybe_unused]] auto& matchController = m_matchController;
    [[maybe_unused]] auto& missionActionSink = m_missionActionSink;
    [[maybe_unused]] auto& missionFactions = m_missionFactions;
    [[maybe_unused]] auto& missionRuntime = m_missionRuntime;
    [[maybe_unused]] auto& missionToLoad = m_missionToLoad;
    [[maybe_unused]] auto& nearSideSurface = m_nearSideSurface;
    [[maybe_unused]] auto& net = m_net;
    [[maybe_unused]] auto& p = m_p;
    [[maybe_unused]] auto& planetR = m_planetR;
    [[maybe_unused]] auto& rotationIndex = m_rotationIndex;
    [[maybe_unused]] auto& terrainStreamer = *m_terrainStreamer;
    [[maybe_unused]] auto& weatherController = *m_weatherController;
    [[maybe_unused]] auto& worldApi = m_worldApi;
    // Where a mission script's require() reads its modules from (#1210). The AI-script cache records
    // each script's pack root as an ASSETS-DOMAIN path, so it must travel with the filesystem that
    // resolves it — reading it any other way resolves it against the process working directory, which
    // is only the content root by accident.
    auto packSourceFor = [&p](std::string rootDir) {
        return fl::ScriptPackSource{std::move(rootDir), p.filesystem.get()};
    };
    // ---- Load the startup mission (#854/#855) ----
    // Resolve the mission asset, hand its bytes to the engine-mission runtime parser (the same schema
    // validate-mission checks), and set up the sim from it BEFORE gameLoop.start(): spawns + factions
    // via applyMission, the coalition registry handed to the broadcaster, and the mission's `player:`
    // objects registered as joinable slots. missionFactions outlives gameLoop (the broadcaster holds a
    // pointer to it), so it is declared here in main's scope.
    // missionFactions is a member (see Impl)
    // Airspace enforcement (#162). Declared beside missionFactions because it holds a reference to it
    // and, like it, must outlive gameLoop (the composite tick hook and the world.* Lua hooks capture
    // it). Inert until a mission supplies zones -- a zoneless server pays one empty-vector check a tick.
    m_alertSystem = std::make_unique<fl::AlertSystem>(missionFactions);
    [[maybe_unused]] auto& alertSystem = *m_alertSystem;
    // The objective/trigger evaluator (#633). Constructed below when a mission loads; declared here so
    // it outlives gameLoop (the broadcaster's tick hook captures a pointer into it).
    // missionRuntime is a member (see Impl)
    // The match-lifecycle state machine (#523). Declared in main scope so it outlives gameLoop (the
    // composite tick hook + the phase/rotate hooks capture references to it). Configured after the team
    // setup; a free-flight/free-for-all leaves it Idle-inert (its hooks never fire a phase change).
    // matchController is a member (see Impl)
    rotationIndex = 0; // current [rotation] item (#523); advanced on match rotate
    // botRoster is a member (see Impl)
    // Mission non-terminal `do:` action sink (#212). The evaluator routes actions like `set_weather storm`
    // here; we point it at the admin command dispatch AFTER the registry is built (below), so a mission
    // can do exactly what an operator could and no more. Set on the sim thread; read on the sim thread
    // (the evaluator steps there). Empty until wired = actions are logged-and-skipped.
    // missionActionSink is a member (see Impl)
    // loadedMissionName is a member (see Impl)
    loadedMissionSpawned = 0;

    // The world.* host seam (#413): the engine integration a Lua AI/mission script reaches through
    // world.spawn/despawn/set_relationship/set_music_state/mission_success/mission_failure. Declared
    // before the mission load so its LuaControllers can bind it; the hooks run on the sim thread (from
    // the controller's sample()), where direct EntityManager/FactionRegistry mutation + a music
    // broadcast are all safe. Lifetime spans gameLoop, so it outlives every LuaController.
    // worldApi is a member (see Impl)
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
    // Airspace posture + zone queries (#162). setAlertLevel goes through AlertSystem rather than
    // straight to the registry, because the system is what fires the change hook that reaches the wire.
    worldApi.setAlertLevel = [&](const std::string& factionId, const std::string& level) {
        fl::AlertLevel lvl{};
        if (!fl::alertLevelFromString(level, lvl))
            return; // unrecognised level name
        alertSystem.setAlertLevel(factionId, lvl);
    };
    worldApi.getAlertLevel = [&](const std::string& factionId) {
        return std::string(fl::alertLevelName(alertSystem.getAlertLevel(factionId)));
    };
    worldApi.getZoneStage = [&](int entityIdx, const std::string& zoneId) {
        return std::string(
            fl::escalationStageName(alertSystem.getIntruderStage(static_cast<uint32_t>(entityIdx), zoneId)));
    };
    worldApi.isInZone = [&](int entityIdx, const std::string& zoneId) {
        return alertSystem.isInZone(static_cast<uint32_t>(entityIdx), zoneId);
    };
    worldApi.setMissionOutcome = [&](bool success) {
        if (missionRuntime)
            missionRuntime->forceOutcome(success);
    };
    // Objective scoring (#1000): a mission trigger's world.score_objective(faction, count) routes into
    // the match controller (count * the mode's points_per_objective, scored only during Active). This is
    // how strike/conquest modes accumulate team score from objectives rather than kills.
    worldApi.scoreObjective = [&](int faction, int count) {
        matchController.recordObjective(static_cast<uint16_t>(faction), count);
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
    // campaignRunner is a member (see Impl)
    // campaignMissionId is a member (see Impl)
    // campaignSavePath is a member (see Impl)
    // campaignYaml is a member (see Impl)
    if (!m_opts.campaign.empty()) {
        if (auto campBytes = fl::loadMissionYaml(m_opts.campaign, &assets, *log)) {
            fl::CampaignParseResult cp = fl::parseCampaign(*campBytes);
            if (!cp.ok) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "campaign '%.80s' failed to parse (%zu error(s))",
                              m_opts.campaign.c_str(), cp.errors.size());
                log->log(LogLevel::Error, __FILE__, __LINE__, buf);
                for (const std::string& e : cp.errors)
                    log->log(LogLevel::Error, __FILE__, __LINE__, e.c_str());
            } else {
                // Theater geographic bounds from each theater manifest (#847). A theater whose manifest
                // is missing/unparseable keeps zero bounds (a warning) — the campaign still runs.
                for (auto& th : cp.campaign.theaters) {
                    auto tf = assets.loadTheater(th.id.c_str());
                    if (!tf || tf->bytes.empty()) {
                        char b[160];
                        std::snprintf(b, sizeof(b), "campaign: theater '%.64s' has no manifest — bounds unset",
                                      th.id.c_str());
                        log->log(LogLevel::Warn, __FILE__, __LINE__, b);
                        continue;
                    }
                    auto tr = fl::parseTheaterManifest(
                        std::string_view(reinterpret_cast<const char*>(tf->bytes.data()), tf->bytes.size()));
                    if (tr.ok)
                        th.bounds = fl::theaterGeoBounds(tr.theater);
                    else
                        log->log(LogLevel::Warn, __FILE__, __LINE__,
                                 ("campaign: theater '" + th.id + "' manifest invalid — bounds unset").c_str());
                }
                // Per-mission/template content: the mission-YAML loader first (builtin id / stem), then a
                // raw pack-relative read for the path-form file references formats.md documents (#847).
                auto content = [&assets, log](const std::string& path) -> std::optional<std::string> {
                    if (auto y = fl::loadMissionYaml(path, &assets, *log))
                        return y;
                    if (auto bytes = assets.loadPackFile(path.c_str()))
                        return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    return std::nullopt;
                };
                // Frontline raster loader (#847): decode the pack-relative 8-bit-grayscale PNG into the
                // pre-sized Frontline. Previously left unset, so campaigns never consumed rasters.
                auto frontlineLoader = [&assets, log](const std::string& path, fl::Frontline& out) -> bool {
                    auto bytes = assets.loadPackFile(path.c_str());
                    if (!bytes) {
                        log->log(LogLevel::Warn, __FILE__, __LINE__,
                                 ("campaign: frontline '" + path + "' not found").c_str());
                        return false;
                    }
                    int w = 0, h = 0;
                    std::vector<uint8_t> pixels = fl::decodeFrontlinePng(bytes->data(), bytes->size(), &w, &h);
                    if (pixels.empty() || w != out.cols() || h != out.rows()) {
                        log->log(LogLevel::Warn, __FILE__, __LINE__,
                                 ("campaign: frontline '" + path + "' decode/dimension mismatch").c_str());
                        return false;
                    }
                    return out.setPixels(std::move(pixels));
                };
                // FNV-1a of the campaign name — stable, replayable. On the standard basis since
                // #1247, so a campaign of a given name draws a different frontline than it did before.
                const uint64_t seed = fl::fnv1a64(cp.campaign.name);
                std::string sanitized;
                for (char ch : cp.campaign.name)
                    sanitized += (std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
                std::error_code ec;
                std::filesystem::create_directories("cache", ec); // the save dir must exist before writeConfigFile
                campaignSavePath = "cache/campaign_" + sanitized + ".flsave";
                campaignRunner =
                    std::make_unique<fl::CampaignRunner>(std::move(cp.campaign), seed, content, frontlineLoader);
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
            std::snprintf(buf, sizeof(buf), "campaign file '%.96s' not found", m_opts.campaign.c_str());
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
            // The mission's authoring frame resolves against THIS server's sphere (#1211).
            fl::MissionParseResult parsed = fl::parseMission(yaml, cfg.world.planetRadiusM);
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
                // two missions differ. FNV-1a over the name; an empty name is still stable, just the
                // bare basis. On the standard basis since #1247, so the rolls differ from before.
                const uint64_t missionSeed = fl::fnv1a64(parsed.mission.name);

                // Per-object controller + loadout attachment (#855). engine-mission spawns the object and
                // calls this back with (id, object); here — where engine-ai / engine-script / the weapon
                // registry are available — we turn `route`/`ai` into a controller and `loadout` into an
                // override. route takes precedence over ai when both are present.
                auto onSpawned = [&, missionSeed](fl::EntityId id, const fl::MissionObject& obj) {
                    std::unique_ptr<fl::IEntityController> ctrl;
                    if (!obj.route.empty()) {
                        std::vector<glm::dvec3> wps;
                        wps.reserve(obj.route.size());
                        for (const auto& wp : obj.route)
                            wps.emplace_back(wp[0], wp[1], wp[2]);
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
                                // The shared ladder (#1236). No ATC service here — see the
                                // atcService note in AiControllerBuild.h for why the mission path
                                // structurally cannot have one at spawn time.
                                fl::AiControllerRequest req;
                                req.luaSource = cacheIt->second.first;
                                req.luaPack = packSourceFor(cacheIt->second.second);
                                req.entityManager = &entityManager;
                                req.worldApi = &worldApi;
                                auto built = fl::buildAiController(req);
                                ctrl = std::move(built.controller);
                                if (built.error == fl::AiBuildError::LuaScriptError) {
                                    char m[224];
                                    std::snprintf(m, sizeof(m), "mission ai: lua script '%.56s' error: %.120s",
                                                  name.c_str(), built.detail.c_str());
                                    log->log(LogLevel::Warn, __FILE__, __LINE__, m);
                                }
                            }
                        } else if (!toks.empty()) {
                            fl::AiControllerRequest req;
                            req.behavior = toks[0];
                            req.args.assign(toks.begin() + 1, toks.end());
                            req.entityManager = &entityManager;
                            req.worldApi = &worldApi;
                            auto built = fl::buildAiController(req);
                            ctrl = std::move(built.controller);
                            if (built.error == fl::AiBuildError::UnknownBehavior) {
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
                                fl::AiControllerRequest req;
                                req.luaSource = cacheIt->second.first;
                                req.luaPack = packSourceFor(cacheIt->second.second);
                                req.entityManager = &entityManager;
                                req.worldApi = &worldApi;
                                auto built = fl::buildAiController(req);
                                ctrl = std::move(built.controller);
                                if (built.error == fl::AiBuildError::LuaScriptError) {
                                    char m[224];
                                    std::snprintf(m, sizeof(m), "mission ai: default lua script '%.48s' error: %.120s",
                                                  def->aiScriptAsset.c_str(), built.detail.c_str());
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
                // The MSL elevation under a world position (#1211): applyMission drops a `start: ground`
                // object onto it along its own radial, which works anywhere on the sphere.
                auto groundHeight = [&](double x, double y, double z) -> double {
                    return terrainStreamer.heightAt(glm::dvec3{x, y, z});
                };
                fl::MissionSetupResult setup =
                    fl::applyMission(parsed.mission, entityManager, missionFactions, &weatherController,
                                     cfg.world.planetRadiusM, onSpawned, groundHeight);
                broadcaster.setFactionRegistry(&missionFactions);

                // ---- Airspace alert system (#162) ----
                // Zones come from the mission; escalation policies from the content pack. Both are
                // installed AFTER applyMission, because a zone's `owner:` resolves against the faction
                // registry applyMission has just populated.
                if (!parsed.mission.airspaceZones.empty()) {
                    std::vector<fl::EscalationPolicy> zonePolicies;
                    const uint32_t packPolicies = fl::registerPackZonePolicies(assets, zonePolicies, *log);
                    for (fl::EscalationPolicy& policy : zonePolicies)
                        alertSystem.addPolicy(std::move(policy));
                    for (const fl::AirspaceZone& z : parsed.mission.airspaceZones)
                        alertSystem.addZone(z);
                    alertSystem.setPlanetRadius(cfg.world.planetRadiusM);

                    // A zone whose owner does not resolve enforces nothing, and that is invisible in
                    // play -- so say so rather than letting the mission look like it is working.
                    for (const std::string& zid : alertSystem.unresolvedZoneIds()) {
                        char zbuf[256];
                        std::snprintf(zbuf, sizeof(zbuf),
                                      "airspace zone '%s' names an owner that is not in the mission's "
                                      "sides list; it will not enforce",
                                      zid.c_str());
                        log->log(LogLevel::Warn, __FILE__, __LINE__, zbuf);
                    }

                    // The zone pass reads a flat POD list rather than EntityManager directly, so
                    // engine-world needs no engine-entity dependency (the ground-elevation query seam).
                    alertSystem.setEntitySampler([&entityManager](std::vector<fl::ZoneEntitySample>& out) {
                        entityManager.forEach([&out](const fl::EntityState& e) {
                            if (e.dead)
                                return;
                            fl::ZoneEntitySample s;
                            s.entityIdx = e.id.index;
                            s.entityGen = static_cast<uint16_t>(e.id.generation);
                            s.factionIndex = e.factionIndex;
                            s.pos = glm::dvec3(e.transform.pos[0], e.transform.pos[1], e.transform.pos[2]);
                            out.push_back(s);
                        });
                    });

                    alertSystem.onAlertLevelChange = [&broadcaster](uint16_t fi, fl::AlertLevel lvl) {
                        broadcaster.broadcastAlertLevelChange(fi, static_cast<uint8_t>(lvl));
                        // Also record it (#600): a posture change is a match event, and the replay
                        // and the agent audit both want it in the same stream as the kills.
                        fl::MatchEvent me;
                        me.type = fl::MatchEventType::AlertLevel;
                        me.factionIndex = fi;
                        me.value = static_cast<int32_t>(lvl);
                        // No tick stamp here, and none needed: the log stamps it (#1076). This caller
                        // is the one that forgot, which is why it no longer can.
                        broadcaster.matchEventLog().append(std::move(me));
                    };
                    alertSystem.onEscalate = [&log](uint32_t entityIdx, const std::string& zoneId,
                                                    fl::EscalationStage stage) {
                        char ebuf[256];
                        std::snprintf(ebuf, sizeof(ebuf), "airspace zone '%s': entity %u -> %s", zoneId.c_str(),
                                      entityIdx, std::string(fl::escalationStageName(stage)).c_str());
                        log->log(LogLevel::Info, __FILE__, __LINE__, ebuf);
                    };

                    char abuf[192];
                    std::snprintf(abuf, sizeof(abuf), "airspace: %zu zone(s), %u pack policy/policies loaded",
                                  alertSystem.zoneCount(), packPolicies);
                    log->log(LogLevel::Info, __FILE__, __LINE__, abuf);
                }

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
                    s.loadout = ps.loadout; // the mission's chosen fit for whoever takes this slot (#1209)
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
                missionRuntime->setOnEnd([log, &broadcaster, &matchController, &campaignRunner, campaignMissionId,
                                          campaignSavePath](const fl::MissionOutcome& o) {
                    const bool success = o.state == fl::MissionState::Complete;
                    // A mission-scripted victory also ends the match, so the server rotates instead of
                    // sitting idle after the objective completes (#523/#584).
                    matchController.forceEnd(std::nullopt);
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
                // Step at the rate the SERVER actually ticks (#1253). Both MissionRuntime and
                // MatchController otherwise fall back to their own `1.0 / 60.0` default -- a second
                // and third answer to "where is 60 decided", reached because nothing was passing the
                // real value in.
                missionRuntime->setSimDt(fl::kServerTickRate.dtSecondsDouble());
                // The per-tick step is wired into the composite match/mission hook below (after the team
                // setup), so the MatchController steps every tick alongside the mission runtime.
                // Bind a pilot's aircraft to its player-slot id on connect (and unbind on disconnect), so
                // destroy(<slot-id>) tracks the live aircraft instead of firing at t=0 (#884). Fired from
                // the handshake on the sim thread — the same thread that steps the runtime.
                m_missionSlotBind = [rt = missionRuntime.get(), &broadcaster](const std::string& id, fl::EntityId eid) {
                    rt->registerObjectEntity(id, eid);
                    // Advertise the late slot<->aircraft bind so a recorder's mission roster stays current (#914).
                    broadcaster.updateMissionRoster(id, eid);
                };
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

    // Match teams (#522): map the game mode's teams onto factions, wire the balancer-driven assigner +
    // switch guard, and apply the mode's friendly-fire override. buildMatchTeams may synthesize + load
    // the FactionRegistry (a zero-pack TDM server with real hostile teams); publish it if so. When the
    // mode is a free-for-all (free-flight / mission sides with no mission) no assigner is installed, so
    // the legacy m_playerFaction behavior is byte-identical.
    {
        fl::MatchTeamSetup teamSetup = fl::buildMatchTeams(gameMode, missionFactions, *log);
        if (teamSetup.synthesizedRegistry)
            broadcaster.setFactionRegistry(&missionFactions);
        if (teamSetup.haveTeams) {
            const std::vector<fl::TeamState> teamTemplate = teamSetup.teams;
            // Live per-team counts, recomputed each call from the peers' current factions.
            auto countTeams = [&broadcaster, teamTemplate]() {
                std::vector<fl::TeamState> teams = teamTemplate;
                broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
                    const uint16_t f = broadcaster.factionForPeer(pi.peerId);
                    for (auto& t : teams)
                        if (t.factionIndex == f)
                            ++t.count;
                });
                return teams;
            };
            broadcaster.setPlayerFaction(teamTemplate.front().factionIndex); // round-robin fallback team
            m_teamAssign = [countTeams](uint32_t) -> std::optional<uint16_t> { return fl::pickTeam(countTeams()); };
            m_teamSwitchAllowed = [countTeams, &broadcaster](uint32_t peerId, uint16_t target) -> bool {
                const std::vector<fl::TeamState> teams = countTeams();
                const uint16_t cur = broadcaster.factionForPeer(peerId);
                const fl::TeamState* from = nullptr;
                const fl::TeamState* to = nullptr;
                for (const auto& t : teams) {
                    if (t.factionIndex == cur)
                        from = &t;
                    if (t.factionIndex == target)
                        to = &t;
                }
                if (!to)
                    return false; // target is not a team
                if (!from)
                    return (to->capacity == 0) || (to->count < to->capacity); // joining from no team
                return fl::switchAllowed(*from, *to);
            };
            char buf[96];
            std::snprintf(buf, sizeof(buf), "match: %zu team(s) balanced by the game mode", teamTemplate.size());
            log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        }
    }
    // Friendly fire: a mode's On/Off override wins over [gameplay] friendly_fire (#522).
    broadcaster.setDamageRules(fl::effectiveDamageRules(gameMode, cfg.gameplay.friendlyFire, cfg.gameplay.crashDamage));

    // ── Match lifecycle (#523) ────────────────────────────────────────────────────────────────
    // Configure the controller for the active mode + teams. Publish state on every phase change (which
    // also toggles the combat freeze) and, on rotation, reset + re-admit the connected pilots. The
    // controller steps every tick from the composite hook below; scoring is fed from the combat path.
    {
        fl::MatchTeamSetup mts = fl::buildMatchTeams(gameMode, missionFactions, *log);
        matchController.configure(gameMode, mts.teams, fl::kServerTickRate.dtSecondsDouble());
        matchController.setEndingSeconds(static_cast<double>(cfg.match.endScreenS));
        broadcaster.setReconnectGraceTicks(static_cast<uint64_t>(std::max(0, cfg.match.reconnectGraceS)) * 60u); // #524

        // Respawn policy from the mode (#648): a dead pilot respawns on request after the delay. Enabled
        // ONLY for a competitive team match — the no-match free-flight default (haveTeams == false) leaves
        // a dead pilot's airframe in place exactly as before #648, so a peer that never requests a respawn
        // (e.g. a load-test bot) is not silently removed from the world. Without this gate, a free-flight
        // pilot's death despawns its entity and drops the m_peerEntities mapping; with nothing to respawn
        // it, the server's entity set drains away under a swarm of crash-and-stay-dead clients.
        if (mts.haveTeams) {
            fl::WorldBroadcaster::RespawnPolicy rp;
            rp.delayTicks = static_cast<uint32_t>(std::max(0.0, gameMode.respawnDelayS) * 60.0);
            rp.waves = gameMode.respawnWaves;
            rp.waveIntervalTicks = static_cast<uint32_t>(std::max(1.0, gameMode.waveIntervalS) * 60.0);
            broadcaster.setRespawnPolicy(rp);
        }

        matchController.setOnPhase([&broadcaster, &matchController, log](fl::MatchPhase /*from*/, fl::MatchPhase to) {
            broadcaster.setCombatFrozen(matchController.combatFrozen());
            char m[64];
            std::snprintf(m, sizeof(m), "match: phase -> %d", static_cast<int>(to));
            log->log(LogLevel::Info, __FILE__, __LINE__, m);
            // The state itself is published by the composite tick hook on the next version change.
        });

        // The rotate hook needs gameLoop.enqueueSimCallback, so it is wired after GameLoop is declared
        // (search "matchController.setOnRotate" below).

        // Feed the controller from the world: kills score, joins/leaves track participants.
        // The scoreboard reads the match event BUS (#1077), not two bespoke sinks. Kills and
        // join/leave were each wired twice before -- a sink call AND a log append -- which is the
        // divergence MatchEventLog's own header predicted and #1076's tick bug was the first instance
        // of. One subscriber, one write path.
        //
        // Notification is synchronous on the sim thread from the damage path, which is what keeps the
        // team-kill test exact: the victim and the killer are both still live entities at that moment,
        // so their factions are there to read. A deferred or queued feed could not do this.
        broadcaster.matchEventLog().subscribe([&matchController, &entityManager](const fl::MatchEvent& ev) {
            switch (ev.type) {
            case fl::MatchEventType::Kill: {
                if (ev.actor == fl::MatchEvent::kNoParticipant && ev.target == fl::MatchEvent::kNoParticipant)
                    return; // neither side is a scoreboard participant (AI vs AI)
                bool sameFaction = false;
                const fl::EntityId victim{ev.subjectIdx, ev.subjectGen};
                if (ev.instigatorIdx != fl::MatchEvent::kNoEntityIdx) {
                    const fl::EntityId killer{ev.instigatorIdx, ev.instigatorGen};
                    if (const fl::EntityState* v = entityManager.get(victim))
                        if (const fl::EntityState* k = entityManager.get(killer))
                            sameFaction = v->factionIndex != 0 && v->factionIndex == k->factionIndex;
                }
                matchController.recordKill(ev.actor, ev.target, sameFaction);
                break;
            }
            case fl::MatchEventType::Join:
                // `value` carries isBot -- see the field comment in MatchEventLog.h.
                matchController.participantJoined(ev.actor, ev.factionIndex, ev.value != 0);
                break;
            case fl::MatchEventType::Leave:
                matchController.participantLeft(ev.actor);
                break;
            default:
                break;
            }
        });

        // AI bot backfill (#87): spawn server-side AI participants up to the fill target. Bots are not
        // network peers. Requires teams (a free-for-all cannot balance bots) and fill > 0.
        if (cfg.bots.fill > 0 && mts.haveTeams) {
            std::vector<uint16_t> botTeams;
            for (const fl::TeamState& ts : mts.teams)
                botTeams.push_back(ts.factionIndex);
            const double botGroundElevM = terrainStreamer.heightAt(nearSideSurface(0.0, 0.0, 0.0));
            std::string fighterSrc(
                fl::builtinAiScript(cfg.bots.aiScript.empty() ? std::string("builtin:fighter") : cfg.bots.aiScript));
            if (fighterSrc.empty())
                fighterSrc = std::string(fl::builtinAiScript("builtin:fighter"));
            const std::string botType = cfg.bots.entityType.empty() ? cfg.world.playerEntityType : cfg.bots.entityType;

            auto botSpawn = [&entityManager, &broadcaster, &worldApi, botGroundElevM, botType, fighterSrc,
                             planetR = planetR, spawnN = uint32_t{0}](uint16_t faction) mutable -> fl::EntityId {
                // Spread around the HOME on the local east/north frame (#1211), 2 km above its ground.
                const double ang = static_cast<double>(spawnN) * 2.399963;
                const double rad = 800.0 + static_cast<double>(spawnN) * 40.0;
                fl::EntityTransform t{};
                t.quat[3] = 1.0f;
                fl::localOffsetToWorld(fl::sandboxHome(), rad * std::cos(ang), rad * std::sin(ang),
                                       botGroundElevM + 2000.0, t.pos[0], t.pos[1], t.pos[2], planetR);
                ++spawnN;
                const fl::EntityId id = entityManager.spawn(botType.c_str(), t);
                if (!id.valid())
                    return {};
                if (fl::EntityState* s = entityManager.get(id); s && faction != 0)
                    s->factionIndex = faction;
                auto ctrl = std::make_unique<fl::LuaController>(fighterSrc, fl::ScriptPackSource{}, &entityManager,
                                                                &worldApi, nullptr);
                if (!ctrl->isValid()) {
                    entityManager.kill(id);
                    return {};
                }
                broadcaster.registerController(id, std::move(ctrl));
                return id;
            };
            auto botKill = [&entityManager](fl::EntityId id) { entityManager.kill(id); };
            auto botAlive = [&entityManager](fl::EntityId id) {
                const fl::EntityState* s = entityManager.get(id);
                return s != nullptr && !s->dead;
            };
            fl::BotRoster::Config bcfg;
            bcfg.fill = cfg.bots.fill;
            bcfg.maxBots = cfg.bots.max;
            bcfg.balanceTeams = cfg.bots.balanceTeams;
            botRoster = std::make_unique<fl::BotRoster>(broadcaster, bcfg, std::move(botTeams), std::move(botSpawn),
                                                        std::move(botKill), std::move(botAlive));
            log->log(LogLevel::Info, __FILE__, __LINE__, "bots: AI backfill enabled");
        }
    }

    // The sim systems are a REGISTERED, ORDERED LIST on the GameLoop (#1078), not a composite lambda
    // behind setMissionTickHook. Registration order below IS execution order, and it reproduces the old
    // lambda's order exactly: the broadcaster first (it is the loop's first system), then the mission
    // runtime, the match controller, airspace enforcement, bot backfill and the world-state bridge.
    //
    // Position in the tick is unchanged. The old hook fired at the very end of WorldBroadcaster::onTick,
    // after the snapshot flush and net.service(); these run immediately after that call returns, which is
    // the same point. Airspace enforcement in particular must stay after the world has stepped, so a zone
    // test sees where everyone actually is this tick rather than where they were last tick.
    //
    // Each system owns its own sub-rate cadence now: BotRoster steps itself at ~1 Hz, WorldStateBridge
    // pushes the mission block every 30 ticks and MsgMatchState on a version change. The caller no longer
    // knows or cares.
    m_worldStateBridge =
        std::make_unique<fl::WorldStateBridge>(broadcaster, matchController, missionRuntime.get(), loadedMissionName);
    [[maybe_unused]] auto& worldStateBridge = *m_worldStateBridge;

    if (!cfg.trace.inputTraceDir.empty()) {
        broadcaster.setInputTraceDir(cfg.trace.inputTraceDir);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "input tracing enabled -> %s", cfg.trace.inputTraceDir.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    if (!cfg.security.banlistPath.empty()) {
        auto banned = fl::loadIpListFile(cfg.security.banlistPath, log);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "banlist: loaded %zu IPs from %s", banned.size(),
                      cfg.security.banlistPath.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        broadcaster.setBannedAddresses(std::move(banned));
    }
    if (!cfg.security.allowlistPath.empty()) {
        auto allowed = fl::loadIpListFile(cfg.security.allowlistPath, log);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "allowlist: loaded %zu IPs from %s", allowed.size(),
                      cfg.security.allowlistPath.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        broadcaster.setAllowedAddresses(std::move(allowed));
    }
    net->setEventHandler(&broadcaster);
    return true;
}

bool ServerRuntime::Impl::initAdmin() {
    // The members this phase touches, bound by name so the moved code reads unchanged --
    // and so its `[&name]` lambda captures still compile: you cannot capture a member.
    [[maybe_unused]] auto& adminChannels = m_adminChannels;
    [[maybe_unused]] auto& adminRegistry = m_adminRegistry;
    [[maybe_unused]] auto& aiScriptCache = m_aiScriptCache;
    [[maybe_unused]] auto& alertSystem = *m_alertSystem;
    [[maybe_unused]] auto& assets = *m_assets;
    [[maybe_unused]] auto& atcService = m_atcService;
    [[maybe_unused]] auto& botRoster = m_botRoster;
    [[maybe_unused]] auto& broadcaster = *m_broadcaster;
    [[maybe_unused]] auto& cfg = m_cfg;
    [[maybe_unused]] auto& churnState = m_churnState;
    [[maybe_unused]] auto& churnTick = m_churnTick;
    [[maybe_unused]] auto& httpAdminServer = m_httpAdminServer;
    [[maybe_unused]] auto& log = m_log;
    [[maybe_unused]] auto& matchController = m_matchController;
    [[maybe_unused]] auto& missionFactions = m_missionFactions;
    [[maybe_unused]] auto& missionRuntime = m_missionRuntime;
    [[maybe_unused]] auto& rconServer = m_rconServer;
    [[maybe_unused]] auto& rotationIndex = m_rotationIndex;
    [[maybe_unused]] auto& worldStateBridge = *m_worldStateBridge;
    // ---- Admin command registry (built before gameLoop to satisfy RAII destruction order) ----
    // Destruction order (LIFO): gameLoop first (sim thread stops), then rconServer
    // (RCON I/O thread stops while adminRegistry still alive), then adminShell, then adminRegistry.
    // adminRegistry is a member (see Impl)
    m_adminShell = std::make_unique<CommandShell>(*log, adminRegistry);
    [[maybe_unused]] auto& adminShell = *m_adminShell;

    // ---- Admin frontends: one AdminChannel each (#1079, D14) ----
    // {auth, issuer resolution, dispatch-with-issuer, drain} behind a constructor, and an enumerable
    // registry so admin_unlock / admin_auth_status reach every channel without naming any of them.
    // Declared HERE, above rconServer/httpAdminServer, so LIFO destruction stops those transports
    // before the channels they dispatch through are gone -- the same rule that puts them above
    // gameLoop. The credential ladder stays with each transport; only the bookkeeping is shared.
    const auto adminDispatch = [&adminRegistry](std::string_view cmd, const fl::CommandIssuer& issuer) {
        // Permission-check the command against the issuer's granted capabilities (#946). A
        // password-authenticated peer arrives with Admin caps and runs everything (the CI path); a
        // grant-channel peer runs only what its caps allow.
        return adminRegistry.dispatch(cmd, issuer);
    };
    const auto channelConfig = [](std::string name, int maxFailures, int lockoutSeconds, bool perIpAuth) {
        fl::AdminChannel::Config c;
        c.name = std::move(name);
        c.maxAuthFailures = maxFailures;
        c.lockoutSeconds = lockoutSeconds;
        c.perIpAuth = perIpAuth;
        return c;
    };
    // adminChannels is a member (see Impl)
    // stdin and the mission `do:` sink present no credential -- their issuer is fl::systemIssuer(),
    // because the operator started this process and chose to load that mission. They are registered
    // anyway: the failure this issue fixes is a surface an operator cannot SEE, and "no per-IP auth"
    // is a thing worth being able to read during an incident.
    m_stdinChannel = std::make_unique<fl::AdminChannel>(adminDispatch, channelConfig("stdin", 0, 0, false),
                                                        fl::SystemClock::instance());
    [[maybe_unused]] auto& stdinChannel = *m_stdinChannel;
    m_missionChannel = std::make_unique<fl::AdminChannel>(adminDispatch, channelConfig("mission", 0, 0, false),
                                                          fl::SystemClock::instance());
    [[maybe_unused]] auto& missionChannel = *m_missionChannel;
    // The enet channel is built in initWorld -- the broadcaster takes it at construction (#1082) --
    // and is null when no operator password is configured, which is what turns that frontend off.
    m_rconChannel = std::make_unique<fl::AdminChannel>(
        adminDispatch, channelConfig("rcon", cfg.rcon.maxAuthFailures, cfg.rcon.lockoutSeconds, true),
        fl::SystemClock::instance());
    [[maybe_unused]] auto& rconChannel = *m_rconChannel;
    // MCP shares this one: same listener, same token table, same lockout (see HttpAdminServer.h).
    m_httpChannel = std::make_unique<fl::AdminChannel>(
        adminDispatch, channelConfig("http", cfg.httpAdmin.maxAuthFailures, cfg.httpAdmin.lockoutSeconds, true),
        fl::SystemClock::instance());
    [[maybe_unused]] auto& httpChannel = *m_httpChannel;
    adminChannels.add(stdinChannel);
    adminChannels.add(missionChannel);

    // Declared here so rconServer outlives gameLoop but is destroyed before adminRegistry.
    // rconServer is a member (see Impl)
    // Same lifetime rule for the REST admin API (#233): its listener thread calls into adminRegistry.
    // httpAdminServer is a member (see Impl)

    // (The read-only AI script cache is built earlier, above the mission load, so the mission's
    // scripted-bot `ai: lua <name>` objects can resolve their scripts — see aiScriptCache.)

    // Projectile-churn state (#580). Declared BEFORE gameLoop: the churn callback re-enqueues a
    // copy of itself each tick, and those queued copies capture this state by reference — it must
    // outlive the sim thread (LIFO destruction: gameLoop's destructor joins the thread first).
    // churnState is a member (see Impl)
    // churnTick is a member (see Impl)

    // Declared before gameLoop so it is destroyed AFTER the loop's sim thread has stopped (the
    // broadcaster holds a raw pointer to it). Constructed + wired below, after gameLoop exists.
    // atcService is a member (see Impl)

    m_gameLoop = std::make_unique<GameLoop>(broadcaster, *log, kSimTickRateHz, cfg.world.maxCatchupTicks);
    [[maybe_unused]] auto& gameLoop = *m_gameLoop;

    // The ordered sim-system list (#1078). Registration order is execution order; see the note above
    // the WorldStateBridge construction. Each of these implements ISimUpdate, so a new sim system is a
    // line here rather than a branch inside a lambda -- and AlertSystem's "why I am not registered with
    // the loop I implement the interface for" comment is gone along with the reason for it.
    if (missionRuntime)
        gameLoop.addSimUpdate(*missionRuntime);
    gameLoop.addSimUpdate(matchController);
    gameLoop.addSimUpdate(alertSystem);
    if (botRoster)
        gameLoop.addSimUpdate(*botRoster);
    gameLoop.addSimUpdate(worldStateBridge);

    // Match rotation (#523): on match end, reset the world, advance to the next rotation item's mode,
    // and re-admit the connected pilots — all serial on the sim thread via enqueueSimCallback (hence
    // wired here, after GameLoop exists). The mission itself repeats (a full mission-YAML reload across
    // rotation is a follow-on; a team match commonly runs without a mission).
    matchController.setOnRotate([&broadcaster, &matchController, &gameLoop, &missionFactions, &rotationIndex, &cfg,
                                 &assets, &botRoster, log]() {
        gameLoop.enqueueSimCallback(
            [&broadcaster, &matchController, &missionFactions, &rotationIndex, &cfg, &assets, &botRoster, log]() {
                if (botRoster)
                    botRoster->clear(); // #87: retire bots before the world reset; refills next step
                broadcaster.resetWorld();
                std::string modeRef = cfg.match.mode;
                if (!cfg.rotation.items.empty()) {
                    rotationIndex = (rotationIndex + 1) % cfg.rotation.items.size();
                    auto [mref, moderef] = fl::splitRotationItem(cfg.rotation.items[rotationIndex]);
                    (void)mref;
                    if (!moderef.empty())
                        modeRef = moderef;
                }
                const fl::GameModeDef nextMode = fl::resolveGameMode(modeRef, &assets, *log);
                fl::MatchTeamSetup nmts = fl::buildMatchTeams(nextMode, missionFactions, *log);
                broadcaster.setDamageRules(
                    fl::effectiveDamageRules(nextMode, cfg.gameplay.friendlyFire, cfg.gameplay.crashDamage));
                matchController.configure(nextMode, nmts.teams, fl::kServerTickRate.dtSecondsDouble());
                if (nmts.haveTeams) { // respawn only for a competitive team match — see the setup path above
                    fl::WorldBroadcaster::RespawnPolicy rp;
                    rp.delayTicks = static_cast<uint32_t>(std::max(0.0, nextMode.respawnDelayS) * 60.0);
                    rp.waves = nextMode.respawnWaves;
                    rp.waveIntervalTicks = static_cast<uint32_t>(std::max(1.0, nextMode.waveIntervalS) * 60.0);
                    broadcaster.setRespawnPolicy(rp);
                } else {
                    broadcaster.disableRespawn(); // rotating into a no-match mode must not keep respawn on
                }
                broadcaster.setCombatFrozen(false);
                broadcaster.readmitPilots();
                log->log(LogLevel::Info, __FILE__, __LINE__, "match: rotated to next round");
            });
    });

    // Data-parallel sim tick: the worker pool that parallelises the per-entity AI + integrate
    // passes. Constructed before gameLoop.start() and outlives it (declared here in main's scope).
    // 0 = auto (hardware_concurrency), 1 = serial. Injected into the broadcaster below.
    m_jobSystem = std::make_unique<fl::JobSystem>(cfg.world.simWorkerThreads);
    [[maybe_unused]] auto& jobSystem = *m_jobSystem;
    broadcaster.setJobSystem(jobSystem);
    {
        char wbuf[96];
        std::snprintf(wbuf, sizeof(wbuf), "sim worker pool: %u background worker(s) (sim_worker_threads=%u)",
                      jobSystem.workerCount(), cfg.world.simWorkerThreads);
        log->log(LogLevel::Info, __FILE__, __LINE__, wbuf);
    }
    return true;
}

bool ServerRuntime::Impl::initSystems() {
    // The members this phase touches, bound by name so the moved code reads unchanged --
    // and so its `[&name]` lambda captures still compile: you cannot capture a member.
    [[maybe_unused]] auto& adminChannels = m_adminChannels;
    [[maybe_unused]] auto& adminCtx = m_adminCtx;
    [[maybe_unused]] auto& adminRegistry = m_adminRegistry;
    [[maybe_unused]] auto& adminShell = *m_adminShell;
    [[maybe_unused]] auto& aiScriptCache = m_aiScriptCache;
    [[maybe_unused]] auto& airportRegistry = m_airportRegistry;
    [[maybe_unused]] auto& assets = *m_assets;
    [[maybe_unused]] auto& atcService = m_atcService;
    [[maybe_unused]] auto& beacon = m_beacon;
    [[maybe_unused]] auto& broadcaster = *m_broadcaster;
    [[maybe_unused]] auto& cfg = m_cfg;
    [[maybe_unused]] auto& churnState = m_churnState;
    [[maybe_unused]] auto& churnTick = m_churnTick;
    [[maybe_unused]] auto& configPath = m_configPath;
    [[maybe_unused]] auto& entityManager = *m_entityManager;
    [[maybe_unused]] auto& entityRegistry = m_entityRegistry;
    [[maybe_unused]] auto& fmCache = m_fmCache;
    [[maybe_unused]] auto& gameLoop = *m_gameLoop;
    [[maybe_unused]] auto& loadedMissionName = m_loadedMissionName;
    [[maybe_unused]] auto& loadedMissionSpawned = m_loadedMissionSpawned;
    [[maybe_unused]] auto& log = m_log;
    [[maybe_unused]] auto& missionActionSink = m_missionActionSink;
    [[maybe_unused]] auto& missionChannel = *m_missionChannel;
    [[maybe_unused]] auto& missionFactions = m_missionFactions;
    [[maybe_unused]] auto& missionRuntime = m_missionRuntime;
    [[maybe_unused]] auto& nearSideSurface = m_nearSideSurface;
    [[maybe_unused]] auto& p = m_p;
    [[maybe_unused]] auto& planetR = m_planetR;
    [[maybe_unused]] auto& primeSpawnHeightUntil = m_primeSpawnHeightUntil;
    [[maybe_unused]] auto& replayRecorder = m_replayRecorder;
    [[maybe_unused]] auto& resolveAiScaling = m_resolveAiScaling;
    [[maybe_unused]] auto& serverUptime = m_serverUptime;
    [[maybe_unused]] auto& weatherController = *m_weatherController;
    [[maybe_unused]] auto& worldApi = m_worldApi;
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
    if (cfg.world.testSpawnAiCount > 0) {
        const double spreadM = cfg.world.testSpawnSpreadKm * 1000.0;

        // Terrain-relative placement (#1137). test_spawn_agl_m used to be measured above the
        // ORIGIN's ground for every entity, so over a wide spread anything above higher ground
        // spawned INSIDE a hill and died on contact — invisible, as a slowly draining population
        // rather than an error, and it made the scale-gate baseline a function of run duration.
        // Each column is primed to heightReadyAt depth before it is sampled; the cost is bounded by
        // the number of distinct tiles under the spread, not by the entity count, because a primed
        // level-10 tile spans ~10 km and covers every later point that lands on it.
        //
        // The shortfall is COUNTED and reported: a spread wide enough to exhaust the budget falls
        // back to whatever coarse height is resident, which is a worse altitude, not a wrong one —
        // and saying so is the difference between a known limitation and the silent drain this
        // replaces.
        // BOUNDED, and bounded tightly: server startup is on a deadline that operators and tests rely
        // on — the replay-determinism test gives fl-server 30 s to reach "listening on", and a 30 s
        // priming budget here blew straight through it on an ASan build, where procedural terrain
        // generation is several times slower. 5 s total, and no single column may eat more than
        // 750 ms of it, so a wide spread degrades to coarse heights (counted and reported below)
        // instead of holding the port closed.
        using namespace std::chrono;
        const auto primeBudget = steady_clock::now() + seconds(5);
        constexpr auto kPerColumnBudget = milliseconds(750);
        uint32_t coarseSpawnPoints = 0;
        const fl::TestSpawnSurfaceFn loadSpawnSurface = [&](double x, double z, double aglM) -> std::array<double, 3> {
            const auto columnDeadline = std::min(primeBudget, steady_clock::now() + kPerColumnBudget);
            if (!primeSpawnHeightUntil(x, z, columnDeadline))
                ++coarseSpawnPoints;
            const glm::dvec3 s = nearSideSurface(x, z, aglM);
            return {s.x, s.y, s.z};
        };
        const auto positions =
            fl::testSpawnPositions(cfg.world.testSpawnAiCount, spreadM, cfg.world.testSpawnAglM, loadSpawnSurface);
        if (coarseSpawnPoints > 0) {
            char cbuf[192];
            std::snprintf(cbuf, sizeof(cbuf),
                          "test spawn: %u of %u spawn points used a COARSE terrain height (streaming budget "
                          "exhausted) — those entities may sit lower above ground than %.0f m",
                          coarseSpawnPoints, cfg.world.testSpawnAiCount, cfg.world.testSpawnAglM);
            log->log(LogLevel::Warn, __FILE__, __LINE__, cbuf);
        }

        // Controller mix (#580): weighted per-index assignment (deterministic, no RNG). Empty mix
        // (the default) = all loiter, keeping the #573 pool+index characterisation baseline intact.
        // Already validated by parseServerConfig — a parse failure here can't happen, but fall back
        // to all-loiter defensively anyway.
        std::vector<fl::TestSpawnMixEntry> mix;
        if (!cfg.world.testSpawnAiMix.empty()) {
            std::string mixErr;
            if (!fl::parseTestSpawnMix(cfg.world.testSpawnAiMix, mix, mixErr))
                mix.clear();
        }

        // #980: the load AI's entity type (default the single-seat debug entity; builtin:bomber runs
        // CREWED AI, exercising the per-seat passes + turret replication under scale-gate load).
        const std::string loadType =
            cfg.world.testSpawnEntityType.empty() ? std::string("builtin:debug-entity") : cfg.world.testSpawnEntityType;
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
                behavior = fl::assignTestSpawnBehavior(mix, spawned, cfg.world.testSpawnAiCount);
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
                      spawned, cfg.world.testSpawnSpreadKm, cfg.world.testSpawnAglM,
                      cfg.world.testSpawnAiMix.empty() ? "loiter" : cfg.world.testSpawnAiMix.c_str());
        log->log(LogLevel::Warn, __FILE__, __LINE__, sbuf);
    }

    // ---- Projectile churn (#580): a self-rearming sim callback spawns testProjectileRate
    // short-lived entities per second and kills each after testProjectileTtlS — sustained
    // spawn+reap traffic through the EntityPool free-list, the O(liveCount) forEach, and the
    // SnapshotDespawn TLV path. Runs on the sim thread (callbacks drain at the top of each tick,
    // before onTick), so spawn/kill need no extra synchronisation. A TESTING AFFORDANCE.
    if (cfg.world.testProjectileRate > 0.0) {
        const double spreadM = cfg.world.testSpawnSpreadKm * 1000.0;
        const double aglM = cfg.world.testSpawnAglM;
        const uint64_t ttlTicks = static_cast<uint64_t>(cfg.world.testProjectileTtlS * 60.0) + 1u;
        const double rate = cfg.world.testProjectileRate;
        // Terrain-relative, same as the load spawn (#1137) — the churn walks the same spread off the
        // same knob, so it had the same trap. Sampled per spawn on the SIM thread: heightAt() is
        // thread-safe (m_tileMutex) and is already the per-tick physics floor, and only update() is
        // main-thread-only. No priming here — the spread's tiles were primed above, and a projectile
        // whose column is not resident gets the coarse height rather than the origin's.
        const fl::TestSpawnSurfaceFn churnSurface = [&nearSideSurface](double x, double z,
                                                                       double agl) -> std::array<double, 3> {
            const glm::dvec3 s = nearSideSurface(x, z, agl);
            return {s.x, s.y, s.z};
        };
        churnTick = [&churnState, &churnTick, &entityManager, &gameLoop, rate, ttlTicks, spreadM, aglM,
                     churnSurface]() {
            ++churnState.tick;
            // Reap expired projectiles (FIFO: deadlines are monotonic).
            while (!churnState.pending.empty() && churnState.pending.front().second <= churnState.tick) {
                entityManager.kill(churnState.pending.front().first);
                churnState.pending.pop_front();
            }
            // Spawn this tick's quota (fractional accumulator carries sub-tick rates).
            const uint32_t n = fl::churnSpawnCount(churnState.spawnAccum, rate, fl::kServerTickRate.dtSecondsDouble());
            for (uint32_t i = 0; i < n; ++i) {
                const auto pos = fl::testProjectilePosition(churnState.spawnCounter++, spreadM, aglM, churnSurface);
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
                      cfg.world.testProjectileRate, cfg.world.testProjectileTtlS,
                      cfg.world.testProjectileRate * cfg.world.testProjectileTtlS);
        log->log(LogLevel::Warn, __FILE__, __LINE__, cbuf);
    }

    // Build the admin command context and FREEZE it: the mutable object never escapes this
    // expression, so there is no "populate, register, then set one more field" shape available
    // afterwards -- that shape is what left `status` reporting the machine's uptime (#1048). Every
    // handler shares this one const instance instead of deep-copying the whole struct.
    adminCtx = std::make_shared<const fl::ServerCommandContext>([&] {
        fl::ServerCommandContext c;
        c.sim.broadcaster = &broadcaster;
        c.sim.entityManager = &entityManager;
        c.sim.typeRegistry = &entityRegistry;
        c.sim.weatherController = &weatherController;
        c.sim.worldApi = &worldApi;   // world.* seam for admin-spawned Lua controllers (#413)
        c.sim.atc = atcService.get(); // atc_status/atc_scramble/atc_hold (#706); null if disabled
        c.env.beacon = beacon.get();
        c.sim.gameLoop = &gameLoop;
        c.env.logger = log;
        c.env.configPath = &configPath;
        c.env.runningConfig = &cfg; // the startup values reload_config diffs against (#1081)
        c.env.quitFlag = &g_quit;
        c.env.uptime = serverUptime; // the process-wide instant, not a fresh one
        c.env.traceDir = cfg.trace.inputTraceDir;
        c.env.resolveAiScaling = resolveAiScaling;
        c.env.loadAIScript = [&aiScriptCache,
                              &p](std::string_view name) -> std::pair<std::string, fl::ScriptPackSource> {
            auto it = aiScriptCache.find(std::string(name));
            if (it == aiScriptCache.end())
                return {};
            // The cached root is an Assets-domain path; it is only resolvable with the filesystem
            // that owns that domain (#1210).
            return {it->second.first, fl::ScriptPackSource{it->second.second, p.filesystem.get()}};
        };
        // reload_content (#152): evict the byte cache + the flight-model resolver cache, then re-resolve
        // every live entity's flight model in place (mass/handling update mid-flight). Runs on the sim
        // thread (the reload_content command enqueues this), where the resolver + m_controlledEntities are
        // owned. Lua-controller live rebuild is a follow-on; flight models are the "feel new handling" case.
        c.env.reloadContent = [&assets, fmCache, &broadcaster]() {
            assets.evictAll();
            fmCache->clear();
            broadcaster.reloadFlightModels();
        };
        c.bans.banlistPath = cfg.security.banlistPath.empty() ? nullptr : &cfg.security.banlistPath;
        c.bans.allowlistPath = cfg.security.allowlistPath.empty() ? nullptr : &cfg.security.allowlistPath;
        c.bans.saveBanlist = [&](const std::unordered_set<std::string>& b) {
            fl::saveIpListFile(cfg.security.banlistPath, b, log);
        };
        c.bans.loadBanlist = [&]() { return fl::loadIpListFile(cfg.security.banlistPath, log); };
        c.bans.loadAllowlist = [&]() { return fl::loadIpListFile(cfg.security.allowlistPath, log); };
        c.shutdown.warningIntervalS = static_cast<uint32_t>(cfg.shutdown.warningIntervalS);
        c.shutdown.minDelayS = static_cast<uint32_t>(cfg.shutdown.minDelayS);
        c.shutdown.requireConfirm = cfg.shutdown.requireConfirm;
        c.rcon.shell = &adminShell;
        // Every frontend at once (#1079). The three per-channel lockout hooks this replaced had to be
        // added by hand for each new frontend, which is how a surface gets forgotten.
        c.adminChannels = &adminChannels;
        return c;
    }());

    fl::registerServerCommands(adminRegistry, adminCtx);

    // Route mission `do:` actions through the validated admin command path (#212), e.g. a trigger
    // `do: set_weather storm` runs the same set_weather command an operator would. dispatch() is const +
    // thread-safe; mutating commands (set_weather/set_time) enqueue onto the sim callback queue, so this
    // is safe to call from the mission evaluator on the sim thread. The result string is logged.
    missionActionSink = [&missionChannel, log](std::string_view action) {
        // Trusted YAML is still an issuer, just a privileged one (#1079): the operator chose to load
        // this mission, so its `do:` actions carry the operator's authority -- explicitly.
        const std::string result = missionChannel.dispatch(std::string(action), fl::systemIssuer());
        char m[288];
        std::snprintf(m, sizeof(m), "mission action '%.120s' -> %.140s", std::string(action).c_str(), result.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, m);
    };

    // MOTD and operator password were applied via applyConfig() above; the ENet admin channel is
    // attached here because its dispatcher needs adminRegistry (built just above). Attaching it is
    // what turns the frontend ON -- with no operator password the broadcaster holds no channel and
    // discards MsgAdminCommand, exactly as an unset dispatcher did before.
    if (!cfg.security.operatorPassword.empty()) {
        m_enetChannel->setShellTap([&adminShell]() { return adminShell.mark(); },
                                   [&adminShell](int m) { return adminShell.drainSince(m); });
        adminChannels.add(*m_enetChannel);
        log->log(LogLevel::Info, __FILE__, __LINE__, "network admin commands: enabled");
    } else {
        log->log(LogLevel::Info, __FILE__, __LINE__,
                 "network admin commands: disabled (no operator_password configured)");
    }

    // ---- Signal handling ----
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // ---- Headless mission run-to-completion (#856) ----
    // With --mission-report, step the sim in a deterministic fixed-step loop (no wall-clock, no sim
    // thread, no networking wait) until the mission ends or a tick cap, write a JSON outcome, and exit.
    // The overrun governor is pinned OFF so AI decimation (aiStride > 1) cannot make the outcome
    // load-dependent — determinism is the entire point of a mission-as-test. Combined with pure Lua
    // scripts (no wall-clock / unseeded RNG), a run is byte-reproducible.
    if (!m_opts.missionReport.empty()) {
        if (!missionRuntime) {
            log->log(LogLevel::Error, __FILE__, __LINE__, "--mission-report requires a mission (use --mission <name>)");
            m_exitCode = 2;
            return false;
        }
        broadcaster.setGovernorParams(fl::makeTickGovernorParams(false, 0.9f, 0.6f, 15.f, 4, 400));
        constexpr double kSimDt = fl::kServerTickRate.dtSecondsDouble();
        constexpr uint64_t kMaxReportTicks = 36000; // 10 sim-minutes at 60 Hz; a stuck mission stops here
        uint64_t ranTicks = 0;
        for (uint64_t tick = 1; tick <= kMaxReportTicks; ++tick) {
            // Run admin-dispatched trigger effects (detonate / atc_scramble / spawn) that the mission's
            // `do:` actions enqueued last tick — the sim thread would drain these at the top of each
            // tick, but this loop drives ticks itself, so drain them here to keep the report faithful.
            gameLoop.drainSimCallbacks();
            // Every registered system, in the loop's own order (#1078). Stepping the broadcaster alone
            // was enough while the mission runtime hung off setMissionTickHook inside it; it is not now,
            // and a report whose tick order differs from production's would not be worth generating.
            gameLoop.stepOnce(kSimDt, tick);
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
        rep.entityCapRefusals = entityManager.softCapRefusals();
        fl::writeConfigFile(m_opts.missionReport, fl::toJson(rep) + "\n", *log);
        char rbuf[256];
        std::snprintf(rbuf, sizeof(rbuf), "mission report: %s after %.1f s / %llu tick(s) -> %s", rep.outcome.c_str(),
                      rep.elapsedSeconds, static_cast<unsigned long long>(rep.ticks), m_opts.missionReport.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, rbuf);
        // --mission-report is done: a successful early exit, not a failure.
        m_exitCode = 0;
        return false;
    }

    // ---- Match recording (#643) ----
    // Started here, last thing before the loop, so the sections describe the world the recording is
    // actually about: every content pack is loaded, every projectile type is registered, and the
    // faction table (if the mission brought one) is final. A `.flrep` whose entity-type manifest was
    // captured earlier would store type indices that mean something else.
    // replayRecorder is a member (see Impl)
    if (cfg.replay.enabled) {
        fl::ReplaySections sections;
        sections.entityTypes.reserve(entityRegistry.typeCount());
        for (uint32_t ti = 0; ti < entityRegistry.typeCount(); ++ti) {
            const fl::EntityDef* def = entityRegistry.byIndex(ti);
            if (!def)
                continue;
            fl::ReplayEntityType t;
            t.typeIndex = ti;
            t.id = def->id;
            t.name = def->name;
            t.category = static_cast<uint8_t>(def->category);
            t.projectileKind = static_cast<uint8_t>(def->projectileKind);
            sections.entityTypes.push_back(std::move(t));
        }
        for (uint16_t fi = 0; fi < missionFactions.count(); ++fi) {
            if (const fl::FactionDef* fd = missionFactions.get(fi)) {
                fl::ReplayFaction f;
                f.factionIndex = fi;
                f.id = fd->id;
                f.name = fd->name;
                sections.factions.push_back(std::move(f));
            }
        }
        // The roster section is empty at this point by construction -- nobody has connected yet.
        // Participants who join later arrive as Join events carrying their callsign, so a reader
        // builds the roster forward as it reads (see WorldBroadcaster::recordParticipant).

        const std::time_t now = std::time(nullptr);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);

        fl::ReplayRecorder::Options ropts;
        ropts.cfg = cfg.replay;
        ropts.baseDir = std::filesystem::current_path();
        ropts.baseName = std::string(stamp) + (loadedMissionName.empty() ? "" : "_" + loadedMissionName);
        ropts.engineVersion = FL_VERSION_STRING;
        ropts.tickRateHz = static_cast<uint32_t>(kSimTickRateHz);
        // Stored, never assumed: every geodetic readout on playback -- and the ACMI export built on
        // this file later (#923) -- is a function of the radius this session actually ran.
        ropts.planetRadiusM = cfg.world.planetRadiusM;
        ropts.missionId = loadedMissionName;
        ropts.startUnixSeconds = static_cast<uint64_t>(now);
        ropts.sessionFlags = loadedMissionName.empty() ? 0u : fl::kReplaySessionMission;

        if (replayRecorder.start(ropts, sections, *log)) {
            m_replayTap = [&replayRecorder, &broadcaster,
                           lastEventSeq = uint64_t{0}](const fl::ReplayTickRecords& rec) mutable {
                // Interleave the events that landed since the previous tick. since() returns
                // copies, so no lock is held while the recorder serializes them.
                std::vector<fl::MatchEvent> events = broadcaster.matchEventLog().since(lastEventSeq);
                if (!events.empty())
                    lastEventSeq = events.back().seq;
                replayRecorder.onTick(rec, std::move(events));
            };
        }
    }
    return true;
}

int ServerRuntime::Impl::mainLoop() {
    // The members this phase touches, bound by name so the moved code reads unchanged --
    // and so its `[&name]` lambda captures still compile: you cannot capture a member.
    [[maybe_unused]] auto& adminChannels = m_adminChannels;
    [[maybe_unused]] auto& adminCtx = m_adminCtx;
    [[maybe_unused]] auto& adminShell = *m_adminShell;
    [[maybe_unused]] auto& aiSinks = m_aiSinks;
    [[maybe_unused]] auto& alertSystem = *m_alertSystem;
    [[maybe_unused]] auto& assets = *m_assets;
    [[maybe_unused]] auto& assetsRoot = m_assetsRoot;
    [[maybe_unused]] auto& beacon = m_beacon;
    [[maybe_unused]] auto& broadcaster = *m_broadcaster;
    [[maybe_unused]] auto& cfg = m_cfg;
    [[maybe_unused]] auto& entityManager = *m_entityManager;
    [[maybe_unused]] auto& gameLoop = *m_gameLoop;
    [[maybe_unused]] auto& httpAdminServer = m_httpAdminServer;
    [[maybe_unused]] auto& httpChannel = *m_httpChannel;
    [[maybe_unused]] auto& httpClient = m_httpClient;
    [[maybe_unused]] auto& listeningMsg = m_listeningMsg;
    [[maybe_unused]] auto& lobbyReg = m_lobbyReg;
    [[maybe_unused]] auto& log = m_log;
    [[maybe_unused]] auto& missionFactions = m_missionFactions;
    [[maybe_unused]] auto& net = m_net;
    [[maybe_unused]] auto& p = m_p;
    [[maybe_unused]] auto& primeSpawnHeight = m_primeSpawnHeight;
    [[maybe_unused]] auto& queryResponder = m_queryResponder;
    [[maybe_unused]] auto& rconChannel = *m_rconChannel;
    [[maybe_unused]] auto& rconServer = m_rconServer;
    [[maybe_unused]] auto& replayRecorder = m_replayRecorder;
    [[maybe_unused]] auto& serverUptime = m_serverUptime;
    [[maybe_unused]] auto& stdinChannel = *m_stdinChannel;
    [[maybe_unused]] auto& stdinReader = m_stdinReader;
    [[maybe_unused]] auto& terrainStreamer = *m_terrainStreamer;
    [[maybe_unused]] auto& userDataRoot = m_userDataRoot;
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
    if (!m_opts.timeRate.empty()) {
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
        if (parseTimeRate(m_opts.timeRate, tr)) {
            gameLoop.setRate(tr);
            log->log(LogLevel::Info, __FILE__, __LINE__, ("time-rate set to " + m_opts.timeRate).c_str());
        } else {
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     ("--time-rate: unknown rate \"" + m_opts.timeRate +
                      "\" (paused|eighth|quarter|half|normal|double|quad|octa); using normal")
                         .c_str());
        }
    }

    // ---- Generative-AI provider seam (#163) ----
    //
    // Always constructed, never null: loadWorldAiProvider hands back a NullAiProvider when the seam
    // is off or a plugin fails, so callers ask supports() rather than checking for null and there is
    // ONE degradation path instead of two.
    std::unique_ptr<fl::IWorldAiProvider> aiProvider;
    // aiSinks is a member (see Impl)
    {
        bool pluginLoadFailed = false;
        const std::string pluginPath = cfg.ai.provider.enabled ? cfg.ai.provider.plugin : std::string{};
        aiProvider = fl::loadWorldAiProvider(pluginPath, *log, pluginLoadFailed);
        if (pluginLoadFailed) {
            // Loud, because the alternative is a server quietly running scripted content for a week
            // because a path was mistyped, with nothing in the log that says so.
            log->log(LogLevel::Error, __FILE__, __LINE__,
                     ("ai_provider: plugin '" + cfg.ai.provider.plugin +
                      "' could not be loaded; continuing with no provider (every AI feature falls back "
                      "to its scripted path)")
                         .c_str());
        }
        if (!aiProvider->init(*log)) {
            const char* why = aiProvider->getLastError();
            log->log(
                LogLevel::Error, __FILE__, __LINE__,
                (std::string("ai_provider: init failed (") + (why ? why : "unknown") + "); continuing with no provider")
                    .c_str());
            aiProvider = std::make_unique<fl::NullAiProvider>();
            (void)aiProvider->init(*log);
        }
        if (cfg.ai.provider.enabled) {
            // Report what it can actually do at startup rather than at first use: an operator who
            // configured a provider for narrative and got one that only maps intent should find out
            // now, not from missing briefings three missions in.
            std::string caps;
            for (uint8_t i = 0; i < static_cast<uint8_t>(fl::WorldAiCapability::Count); ++i) {
                const auto c = static_cast<fl::WorldAiCapability>(i);
                if (aiProvider->supports(c))
                    caps += (caps.empty() ? "" : ", ") + std::string(fl::worldAiCapabilityName(c));
            }
            log->log(LogLevel::Info, __FILE__, __LINE__,
                     ("ai_provider: " + (caps.empty() ? std::string("no capabilities (scripted fallback everywhere)")
                                                      : "capabilities: " + caps))
                         .c_str());
        }

        // The sinks a WorldEvolutionDelta is applied through. Each returns false when the change
        // does not hold, and applyWorldEvolution turns that into a counted, reported rejection.
        aiSinks.factionCount = [&missionFactions]() -> uint16_t { return missionFactions.count(); };
        aiSinks.setAlertLevel = [&](uint16_t idx, fl::AlertLevel level) {
            // Through AlertSystem, not straight to the registry: the system is what fires the change
            // hook that reaches the wire, so a delta applied around it would be invisible to clients.
            const fl::FactionDef* def = missionFactions.get(idx);
            if (!def)
                return false;
            alertSystem.setAlertLevel(def->id, level);
            return true;
        };
        aiSinks.setRelationship = [&](uint16_t a, uint16_t b, fl::FactionRelation rel) {
            missionFactions.setRelationship(a, b, rel);
            return true;
        };
        // setZoneOwner is deliberately LEFT NULL: zone ownership comes from the mission's
        // `airspace_zones:` section, which MissionParser owns, and AlertSystem has no runtime setter
        // for it. A zone_control change is therefore rejected with a reason rather than silently
        // dropped. Wiring it is a follow-on that has to decide what re-owning a live zone means.
        // spawn likewise stays null until a delta-driven spawn has a home for its controller.
    }

    // ---- Chat-to-intent bridge (#611) ----
    //
    // Team chat -> a templated prompt -> the provider -> a grammar name -> the SAME order path the
    // radio menu drives. The model chooses among validated commands; it never invents an action and
    // never supplies a target.
    if (cfg.ai.chatIntent.enabled && aiProvider->supports(fl::WorldAiCapability::Intent)) {
        // Per-peer, per-minute budget. Deliberately separate from (and far below) the chat rate
        // limit: a chat line costs nothing and a model call costs money and latency, so the team
        // channel must not be a lever against the server's own inference budget.
        struct IntentBudget {
            std::chrono::steady_clock::time_point windowStart{};
            int count{0};
            bool warned{false};
        };
        auto budgets = std::make_shared<std::unordered_map<uint32_t, IntentBudget>>();
        const std::string systemPrompt = fl::ai::buildIntentSystemPrompt();

        // A bus subscriber (#1077), not a hook: this tier OBSERVES team chat. The Chat record is
        // appended after the moderation veto, so a suppressed line still never reaches a model, and the
        // channel filter lives here -- with the policy -- rather than inside the broadcaster.
        broadcaster.matchEventLog().subscribe([&, budgets, systemPrompt](const fl::MatchEvent& ev) {
            if (ev.type != fl::MatchEventType::Chat ||
                static_cast<fl::ChatChannel>(ev.channel) != fl::ChatChannel::Team)
                return;
            const uint32_t peerId = ev.actor;
            const std::string_view text = ev.text;
            // Cheap local gate first: most team chat is not addressed to the flight, and asking a
            // model to classify every word said in a match would be both expensive and pointless.
            if (!fl::ai::looksLikeWingmanAddress(text))
                return;

            auto& b = (*budgets)[peerId];
            const auto now = std::chrono::steady_clock::now();
            if (now - b.windowStart >= std::chrono::minutes(1)) {
                b.windowStart = now;
                b.count = 0;
                b.warned = false;
            }
            if (cfg.ai.chatIntent.rateLimitPerMin > 0 && ++b.count > cfg.ai.chatIntent.rateLimitPerMin) {
                if (!b.warned && cfg.ai.chatIntent.notifyOnDecline) {
                    b.warned = true; // once per window; a flood must not be amplified back at the sender
                    broadcaster.sendNoticeTo(peerId, "Voice/chat orders are rate limited; use the radio menu.");
                }
                return;
            }

            fl::WorldAiContext ctx;
            ctx.snapshot = broadcaster.worldStatePublisher().get();
            ctx.utterance = std::string(text);
            ctx.operatorHint = systemPrompt;

            const fl::WorldAiRequestId id =
                aiProvider->requestIntent(ctx, [&, peerId](std::string name, std::string error) {
                    // Fires on the MAIN thread (provider->service()), so nothing here may touch the sim
                    // directly. Validate here — pure — then hand the ordinal to the sim thread.
                    if (!error.empty())
                        return;
                    const fl::ai::IntentResult r = fl::ai::validateIntentResponse(name);
                    if (!r.command) {
                        if (cfg.ai.chatIntent.notifyOnDecline && r.rejection != fl::ai::IntentRejection::Declined) {
                            // A DECLINE is the model behaving correctly and needs no apology; a malformed
                            // or out-of-grammar answer is a backend problem the operator should see.
                            char nbuf[128];
                            std::snprintf(nbuf, sizeof(nbuf), "intent mapping rejected (%.*s)",
                                          static_cast<int>(fl::ai::intentRejectionName(r.rejection).size()),
                                          fl::ai::intentRejectionName(r.rejection).data());
                            log->log(LogLevel::Warn, __FILE__, __LINE__, nbuf);
                        }
                        return;
                    }
                    const auto ordinal = static_cast<uint8_t>(*r.command);
                    gameLoop.enqueueSimCallback([&broadcaster, peerId, ordinal] {
                        // THE SAME call the wire path makes. Authority, target designation, dispatch and
                        // the ack to the commander are all the scripted path's, unchanged.
                        (void)broadcaster.issueWingmanOrder(peerId, ordinal);
                    });
                });
            if (id == 0 && cfg.ai.chatIntent.notifyOnDecline)
                broadcaster.sendNoticeTo(peerId, "Voice/chat orders are unavailable; use the radio menu.");
        });
        log->log(LogLevel::Info, __FILE__, __LINE__,
                 "ai.chat_intent: free-text wingman commands enabled over team chat");
    } else if (cfg.ai.chatIntent.enabled) {
        log->log(LogLevel::Warn, __FILE__, __LINE__,
                 "ai.chat_intent is enabled but the provider does not support intent mapping; "
                 "the radio menu remains the path");
    }

    // ---- REST admin API + health probe (#233) ----
    // Started before RCON only so the two log lines read in config order; they are independent.
    if (cfg.httpAdmin.enabled) {
        httpAdminServer = std::make_unique<fl::HttpAdminServer>(httpChannel, cfg.httpAdmin, *log, serverUptime);
        adminChannels.add(httpChannel);

        // ---- MCP surface (#601) ----
        // A second frontend on the listener above, so it is enabled before start() rather than
        // started separately. The three hooks are all it needs from the sim.
        if (cfg.ai.mcp.enabled) {
            fl::McpHooks hooks;
            hooks.auditAgentAction = [&broadcaster](std::string_view tool, const fl::CommandIssuer& issuer,
                                                    std::string_view detail) {
                // Into the ONE match event log (plan D1), which the replay recorder already
                // interleaves into every .flrep — so an agent's actions are in the recording of the
                // match they affected, not in a side channel someone has to think to collect.
                fl::MatchEvent ev;
                ev.type = fl::MatchEventType::AgentAction;
                ev.actor = issuer.peerId;
                ev.factionIndex = issuer.factionIndex;
                // The log stamps the tick (#1076). This path used to copy the PUBLISHED snapshot's
                // tick, because m_currentTick is sim-thread-only and this runs on an HTTP thread —
                // which lagged by up to the ~1 Hz republish interval. The log's own tick is an
                // atomic, so it reads a value at most one tick old from any thread.
                ev.text = std::string(tool) + " " + std::string(detail);
                broadcaster.matchEventLog().append(std::move(ev));
            };
            hooks.worldStateTick = [&broadcaster]() -> uint64_t {
                const auto snap = broadcaster.worldStatePublisher().get();
                return snap ? snap->tick : 0;
            };
            hooks.matchEventSeq = [&broadcaster]() -> uint64_t { return broadcaster.matchEventLog().nextSeq(); };
            httpAdminServer->enableMcp(cfg.ai.mcp, std::move(hooks));
        }

        if (!httpAdminServer->start()) {
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "http_admin failed to start; continuing without the REST admin API");
            httpAdminServer.reset();
        }
    }

    // ---- RCON server (optional TCP remote admin channel) ----
    if (cfg.rcon.enabled) {
        rconChannel.setShellTap([&adminShell]() { return adminShell.mark(); },
                                [&adminShell](int m) { return adminShell.drainSince(m); });
        rconServer = std::make_unique<fl::RconServer>(rconChannel, cfg.rcon, *log);
        adminChannels.add(rconChannel);
        if (!rconServer->start()) {
            log->log(LogLevel::Warn, __FILE__, __LINE__, "RCON server failed to start; continuing without RCON");
            rconServer.reset();
        }
    }

    // ---- Admin console (stdin command loop) ----
    // The reader owns its thread and is stopped before main() returns (#1038). It reads fd 0 / the
    // console handle directly rather than std::cin, so it never holds the C-stdio lock that
    // exit-time flushing waits on -- the deadlock that used to wedge both `quit` and Ctrl-C for any
    // parent that keeps stdin open.
    // stdinReader is a member (see Impl)
    stdinReader.start();
    std::vector<std::string> stdinLines;

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

    // Asset hot-reload (#152): the automatic server-side watcher. Opt-in via FL_HOT_RELOAD=1 (the
    // client sets it and the LocalServer subprocess inherits it, so single-player lights up both
    // ends). When a flight-model file changes, the authoritative sim re-resolves it onto every live
    // entity in place — "edit the TOML, save, feel the new handling" with no console command. Mesh /
    // texture / livery changes are the client's concern; the server holds no GPU caches.
    std::unique_ptr<fl::StdFilesystemWatcher> contentWatcher;
    if (const char* hr = std::getenv("FL_HOT_RELOAD"); hr && std::strcmp(hr, "1") == 0) {
        contentWatcher = std::make_unique<fl::StdFilesystemWatcher>(assetsRoot, userDataRoot, /*pollIntervalMs=*/250,
                                                                    /*maxFilesPerWatch=*/20000, log);
        assets.enableHotReload(*contentWatcher);
        log->log(LogLevel::Info, __FILE__, __LINE__, "hot-reload enabled (FL_HOT_RELOAD=1)");
    }

    uint64_t lastDroppedTicks = 0; // for the sim-overrun drop-rate Warn (#514)
    // Baseline RSS captured once after all init, before the main loop; the soak leak gate tracks
    // the growth (rss_kb - rss_startup_kb) over the run (#707). 0 when unavailable on this platform.
    const uint64_t rssStartupKb = fl::currentRssKb();
    while (!g_quit) {
        {
            stdinLines.clear();
            stdinReader.drain(stdinLines);
            for (const std::string& line : stdinLines) {
                std::string result = stdinChannel.dispatch(line, fl::systemIssuer());
                if (!result.empty())
                    std::printf("[admin] %s\n", result.c_str());
            }
        }
        {
            const fl::WorldBroadcaster::ShutdownStatus ss = broadcaster.getShutdownStatus(); // #226
            if (beacon)
                beacon->tick({broadcaster.getPeerCount(), ss.active,
                              static_cast<uint16_t>(std::min<uint32_t>(ss.secondsRemaining, 0xFFFFu))});
            if (queryResponder) // #997: refresh the query responder's dynamic snapshot
                queryResponder->setDynamicInfo(
                    {static_cast<uint8_t>(std::min(broadcaster.getPeerCount(), 255)), ss.active,
                     static_cast<uint16_t>(std::min<uint32_t>(ss.secondsRemaining, 0xFFFFu))});
        }
        if (lobbyReg) { // #143: heartbeat the lobby registration with the live player count
            lobbyReg->setDynamic(static_cast<int>(broadcaster.getPeerCount()), std::string{});
            lobbyReg->tick();
        }
        if (httpClient)
            httpClient->service();

        p.asyncFilesystem->service();

        // Hot-reload poll (#152): map events on this (main) thread, then apply on the sim thread. Only
        // a flight-model change needs the authoritative live re-apply; ignore the rest (the server has
        // no GPU caches). applyContentReload evicts + reloadFlightModels; it is the same lambda the
        // reload_content admin command runs.
        if (contentWatcher) {
            auto events = contentWatcher->pollEvents();
            if (!events.empty()) {
                auto changed = assets.mapEventsToAssets(events);
                const bool fmChanged = std::any_of(changed.begin(), changed.end(), [](const fl::ChangedAsset& c) {
                    return c.type == fl::AssetType::FlightModel;
                });
                if (fmChanged && adminCtx->env.reloadContent)
                    gameLoop.enqueueSimCallback([adminCtx]() { adminCtx->env.reloadContent(); });
            }
        }

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
            // #576: per-peer throttle attribution, in ascending peerId so two runs of the same
            // session produce byte-comparable metrics files.
            std::vector<fl::ServerTickReport::PeerThrottle> throttles;
            broadcaster.forEachPeer([&throttles](const fl::PeerInfo& pi) {
                if (!pi.governorBinding && !pi.congestionBinding)
                    return; // a peer at full rate has nothing to attribute
                throttles.push_back({pi.peerId, static_cast<double>(pi.sendRateHz), pi.effectiveIntervalTicks,
                                     pi.governorBinding, pi.congestionBinding, static_cast<double>(pi.packetLoss)});
            });
            std::sort(throttles.begin(), throttles.end(),
                      [](const auto& a, const auto& b) { return a.peerId < b.peerId; });

            fl::ServerTickReport rep = fl::makeServerTickReport(
                broadcaster.getTickBudget(), broadcaster.getPeerCount(), entityManager.liveCount(), ov.loadFactor,
                droppedTicks, fl::currentRssKb(), rssStartupKb, ov.interestScale, ct.minSendHz, ct.recoveredSendHz,
                ct.maxPacketLoss, wt.outKbs, wt.inKbs, wt.outPacketsPerSec, wt.peersAtSample, entityManager.softCap(),
                entityManager.softCapRefusals());
            rep.peerThrottle = std::move(throttles);
            rep.voiceRelaySends = broadcaster.voiceRelaySendCount(); // #1090: fan-out, not frame count
            fl::writeConfigFile(metricsPath, fl::toJson(rep) + "\n", *log);
            nextMetricsWrite = std::chrono::steady_clock::now() + metricsInterval;
        }

        // Drain AI completions on THIS thread (#163). Unconditional: NullAiProvider's service() is
        // empty, and branching here to save an empty call would put a condition in the main loop for
        // nothing.
        aiProvider->service();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Cancel in-flight model calls and join before anything they might touch goes away. Before the
    // lobby deregistration below for the same reason ordering matters at all here: a callback firing
    // into a half-torn-down server is a crash in shutdown, which is the hardest kind to reproduce.
    aiProvider->shutdown();

    // #143: drop the lobby entry on shutdown (best-effort DELETE + one service pass to send it).
    if (lobbyReg)
        lobbyReg->deregister();
    if (httpClient) {
        httpClient->service();
        httpClient->shutdown();
    }

    // Flush and close the recording before anything else tears down: the writer thread is holding
    // the tail of the match (#643).
    replayRecorder.stop();

    // Stop the console reader BEFORE the rest of the teardown: it is the one thread that outlived
    // main() and wedged exit (#1038). Everything below this line already joined its own threads.
    stdinReader.stop();

    if (httpAdminServer)
        httpAdminServer->stop();
    if (rconServer)
        rconServer->stop();
    gameLoop.stop();
    p.asyncFilesystem->shutdown(); // join worker thread before TerrainStreamer destructs

    log->log(LogLevel::Info, __FILE__, __LINE__, "shutting down");
    net->disconnect();
    net->shutdown();

    return 0;
}

ServerRuntime::ServerRuntime(Options options) : m_opts(std::move(options)), m_impl(std::make_unique<Impl>(m_opts)) {}

// Out of line because Impl is only complete here -- which is the point: the teardown order lives
// beside the phases that build the objects, not in a header half the server includes.
ServerRuntime::~ServerRuntime() = default;

int ServerRuntime::run() {
    // A phase returning false has already logged why and set the code. That covers both a fatal
    // error and a successful early finish (--mission-report), which are the same control flow.
    if (!m_impl->initConfig() || !m_impl->initNet() || !m_impl->initContent() || !m_impl->initWorld() ||
        !m_impl->initMission() || !m_impl->initAdmin() || !m_impl->initSystems())
        return m_impl->m_exitCode;
    return m_impl->mainLoop();
}

} // namespace fl
