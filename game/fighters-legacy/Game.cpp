// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#include "Game.h"

#include "CameraInput.h"
#include "ClientNetEventHandler.h"
#include "DebriefScreen.h"
#include "ENetNetwork.h"
#include "FileLogger.h"
#include "FlightInputCollector.h"
#include "FlightScreen.h"
#include "HapticController.h"
#include "IWindowEventHandler.h"
#include "LocalServer.h"
#include "Platform.h"
#include "PrecipitationController.h"
#include "ScreenManager.h"
#include "ServerNotice.h"
#include "SessionStatus.h"
#include "Version.h"
#include "audio/MusicManager.h"
#include "audio/PlaylistLoader.h"
#include "audio/SubtitleQueue.h"
#include "config/UserConfig.h"
#include "console/CommandRegistry.h"
#include "console/GameConsole.h"
#include "content/AssetManager.h"
#include "content/ModLoader.h"
#include "crash/CrashInfo.h"
#include "crash/CrashReporter.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
#include "firstrun/FirstRun.h"
#include "net/DiscoveryListener.h"
#include "net/GameProtocol.h"
#include "openal/OALAudio.h"
#include "perf/PerformanceOverlay.h"
#include "render/BuiltinGeometry.h"
#include "render/CameraController.h"
#include "render/FlightHud.h"
#include "render/IHud.h"
#include "render/ParticleSystem.h"
#include "render/RenderSnapshot.h"
#include "render/SceneRenderer.h"
#include "render/SimRenderBridge.h"
#include "render/TerrainStreamer.h"
#include "render/WindshieldRain.h"
#include "sandbox/SandboxInspector.h"
#include "sdl3/SDL3AsyncFilesystem.h"
#include "sdl3/SDL3Cursor.h"
#include "sdl3/SDL3Display.h"
#include "sdl3/SDL3Filesystem.h"
#include "sdl3/SDL3Input.h"
#include "sdl3/SDL3Joystick.h"
#include "sdl3/SDL3Window.h"
#include "vulkan/VkRendererFactory.h"

#include "ConnectArgs.h"
#include "console/ConsoleCommands.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// File-scope helpers
// ---------------------------------------------------------------------------

static std::function<void(std::string_view)> makeNetworkAdminSender(INetwork& net, std::string token) {
    return [&net, tok = std::move(token)](std::string_view cmd) {
        fl::MsgAdminCommand msg{};
        msg.msgId = static_cast<uint8_t>(fl::MsgId::AdminCommand);
        std::size_t plen = std::min(tok.size(), sizeof(msg.token) - 1u);
        std::memcpy(msg.token, tok.c_str(), plen);
        msg.token[plen] = '\0';
        std::size_t clen = std::min(cmd.size(), sizeof(msg.command) - 1u);
        std::memcpy(msg.command, cmd.data(), clen);
        msg.command[clen] = '\0';
        net.send(0, &msg, sizeof(msg), /*reliable=*/true);
    };
}

static RendererSettings buildRendererSettings(const GraphicsSettings& g) {
    RendererSettings s{};
    switch (g.vsync) {
    case VsyncMode::Off:
        s.vsync = RendererVsyncMode::Off;
        break;
    case VsyncMode::Adaptive:
        s.vsync = RendererVsyncMode::Adaptive;
        break;
    default:
        s.vsync = RendererVsyncMode::On;
        break;
    }
    s.antiAliasing = g.antiAliasing;
    s.bloom = (g.qualityPreset >= QualityLevel::Medium);
    switch (g.drawDistance) {
    case DrawDistance::Low:
        s.drawDistanceKm = 15.0f;
        break;
    case DrawDistance::Medium:
        s.drawDistanceKm = 30.0f;
        break;
    case DrawDistance::Ultra:
        s.drawDistanceKm = 100.0f;
        break;
    default:
        s.drawDistanceKm = 50.0f;
        break; // High
    }
    return s;
}

static constexpr float kSnowAltitudeThresholdM = 2000.0f;

static const fl::EntityRenderEntry* findPlayerEntry(const fl::SimRenderBridge& bridge, uint32_t idx, uint32_t gen) {
    if (!bridge.hasSnapshot())
        return nullptr;
    for (const auto& e : bridge.current().entries)
        if (e.entityIdx == idx && e.entityGen == gen)
            return &e;
    return nullptr;
}

static void updateAudioListener(IAudio& audio, const CameraView& cam, const glm::vec3& vel) {
    const glm::vec3 fwd = -glm::vec3(cam.view[2][0], cam.view[2][1], cam.view[2][2]);
    const glm::vec3 up = glm::vec3(cam.view[1][0], cam.view[1][1], cam.view[1][2]);
    const float pos[3] = {static_cast<float>(cam.worldOrigin.x), static_cast<float>(cam.worldOrigin.y),
                          static_cast<float>(cam.worldOrigin.z)};
    const float fwdA[3] = {fwd.x, fwd.y, fwd.z};
    const float upA[3] = {up.x, up.y, up.z};
    const float velA[3] = {vel.x, vel.y, vel.z};
    audio.setListenerTransform(pos, fwdA, upA);
    audio.setListenerVelocity(velA);
}

static void updatePerfOverlay(GameConsole& console, IRenderer& renderer, PerformanceOverlay& overlay,
                              const fl::SimRenderBridge& bridge, UserConfig& userConfig, bool inFlight) {
    if (!inFlight) {
        overlay.setMode(OverlayMode::Off);
        renderer.setOverlayLines({});
        return;
    }

    const bool* keys = SDL_GetKeyboardState(nullptr);
    static bool f3Prev = false;
    if (!console.isOpen() && keys[SDL_SCANCODE_F3] && !f3Prev) {
        overlay.cycleMode();
        DebugSettings ds = userConfig.debug();
        ds.overlayMode = overlay.mode();
        userConfig.setDebug(ds);
        userConfig.save();
    }
    f3Prev = keys[SDL_SCANCODE_F3];

    const uint32_t entityCount = bridge.hasSnapshot() ? static_cast<uint32_t>(bridge.current().entries.size()) : 0u;
    overlay.update(renderer.getFrameStats(), entityCount, 1000.0f / 60.0f);
    renderer.setOverlayLines(overlay.lines());
}

struct ResizeHandler : IWindowEventHandler {
    IRenderer* r = nullptr;
    void onResize(int w, int h) override {
        r->onResize(w, h);
    }
    void onClose() override {}
};

static void registerBuiltinParticlePresets(fl::ParticleSystem& ps) {
    ps.registerPreset("explosion", {200.0f, 1.5f, 15.0f, {1.0f, 0.6f, 0.1f}, {0.4f, 0.2f, 0.1f}, 0.3f, 3.0f, true});
    ps.registerPreset("fire", {120.0f, 2.0f, 8.0f, {1.0f, 0.4f, 0.05f}, {0.6f, 0.1f, 0.0f}, 0.2f, 1.5f, true});
    ps.registerPreset("smoke", {60.0f, 4.0f, 3.0f, {0.4f, 0.4f, 0.4f}, {0.15f, 0.15f, 0.15f}, 0.5f, 3.0f, false});
    ps.registerPreset(
        "rain",
        {100.0f, 1.5f, 40.0f, {0.5f, 0.6f, 0.8f}, {0.3f, 0.4f, 0.6f}, 0.05f, 0.05f, false, {0.0f, -1.0f, 0.0f}, 20.0f});
    ps.registerPreset(
        "storm_rain",
        {200.0f, 1.2f, 50.0f, {0.6f, 0.7f, 0.9f}, {0.3f, 0.4f, 0.6f}, 0.08f, 0.08f, false, {0.0f, -1.0f, 0.0f}, 20.0f});
    ps.registerPreset("snow", {200.0f,
                               6.0f,
                               2.0f,
                               {0.9f, 0.95f, 1.0f},
                               {0.85f, 0.90f, 1.0f},
                               0.15f,
                               0.10f,
                               false,
                               {0.0f, -1.0f, 0.0f},
                               80.0f});
    ps.registerPreset("storm_snow", {400.0f,
                                     6.0f,
                                     4.0f,
                                     {0.9f, 0.95f, 1.0f},
                                     {0.85f, 0.90f, 1.0f},
                                     0.12f,
                                     0.08f,
                                     false,
                                     {0.0f, -1.0f, 0.0f},
                                     80.0f});
}

// ---------------------------------------------------------------------------
// GameImpl — holds all game state
// ---------------------------------------------------------------------------

// Stable, init-time systems that live for the whole program (built once in Game::init, reused
// across sessions). Platform is declared first → destroyed last, so its logger (rawLogger) stays
// valid throughout the destruction of all other members.
struct GameServices {
    Platform p;
    FileLogger* rawLogger{nullptr};
    fs::path userDataDir;
    fs::path assetsRoot;

    // Crash reporting
    CrashInfo crashInfo;
    CrashReporter crashReporter;
    bool crashReporterReady{false};

    // Config + renderer settings
    std::optional<UserConfig> userConfig;
    RendererSettings rendererSettings;
    ResizeHandler resizeHandler;

    // Content
    std::unique_ptr<AssetManager> assets;
    FirstRunOutcome outcome{};

    // Core game systems
    fl::EntityTypeRegistry entityRegistry;
    fl::SimRenderBridge renderBridge;
    fl::ParticleSystem particleSystem;
    fl::CameraController cameraController;
    std::unique_ptr<fl::SceneRenderer> sceneRenderer;
    std::unique_ptr<fl::TerrainStreamer> terrainStreamer;
    SubtitleQueue subtitleQueue;
    MusicManager musicManager;

    // Multiplayer connection target (empty = single-player, spawn LocalServer).
    // Populated from --connect CLI arg in initPlatform().
    std::string connectHost;
    uint16_t connectPort{4778};
    std::string operatorPassword; // merged: CLI arg > FL_OPERATOR_PASSWORD > [client].operator_password

    // HUD / overlays
    EnvironmentState env;
    fl::FlightHud flightHud;
    fl::IHud* activeHud{nullptr};
    fl::WindshieldRain windshieldRain;
    ServerNotice serverNotice;

    // Debug console
    CommandRegistry cmdRegistry;
    std::optional<GameConsole> gameConsole;

    // Per-frame state
    CameraInput camInput;
    PerformanceOverlay perfOverlay;
    FlightInputCollector flightInput;
    PrecipitationController precipController;

    // Screen state machine
    std::unique_ptr<ScreenManager> screenMgr;
};

// Per-session objects — created in startGame(), torn down in stopGame(). Hold pointers/refs into
// GameServices, so they are destroyed first (declared after services in GameImpl); stopGame() also
// empties them before ~GameImpl, so the program-exit destruction order is moot.
struct SessionContext {
    std::optional<LocalServer> localServer;
    std::unique_ptr<ENetNetwork> clientNet;
    std::unique_ptr<ClientNetEventHandler> clientHandler;
    std::optional<HapticController> hapticController;
    std::optional<DiscoveryListener> discoveryListener;
    std::optional<SandboxInspector> inspector;

    std::thread serverThread;
    std::atomic<bool> serverReady{false};
    // Typed session failure, first-writer-wins (server thread + ClientNetEventHandler write;
    // LoadingScreen reads). Replaces the prior two atomic<const char*> + static-string signals.
    std::atomic<SessionFailure> sessionFailure{SessionFailure::None};
};

struct GameImpl {
    GameServices services;
    SessionContext session;
};

// ---------------------------------------------------------------------------
// Game
// ---------------------------------------------------------------------------

Game::Game() = default;

Game::~Game() {
    if (!m_impl)
        return;
    auto& d = *m_impl;
    // Tear down any active session (joins server thread, disconnects ENet).
    if (d.session.serverThread.joinable() || d.session.clientNet || d.session.localServer)
        stopGame();
    d.services.musicManager.shutdown();
    d.services.p.cursor.reset();
    if (d.services.p.audio)
        d.services.p.audio->shutdown();
    if (d.services.p.renderer)
        d.services.p.renderer->shutdown();
    if (d.services.p.window)
        d.services.p.window->shutdown();
    if (d.services.crashReporterReady)
        d.services.crashReporter.shutdown();
}

bool Game::init(int argc, char** argv) {
    m_impl = std::make_unique<GameImpl>();
    if (!initPlatform(argc, argv))
        return false;
    if (!initWindowAndRenderer())
        return false;
    if (!initContent())
        return false;
    initGameSystems();
    initGameConsole();
    initScreenManager();
    return true;
}

// Steps 1–7: logger, filesystem, user config, audio, input.
bool Game::initPlatform(int argc, char** argv) {
    auto& d = *m_impl;

    SDL_Init(0);
    char* prefRaw = SDL_GetPrefPath("jomkz", "fighters-legacy");
    d.services.userDataDir = prefRaw ? fs::path(prefRaw) : fs::path(".");
    if (prefRaw)
        SDL_free(prefRaw);

    auto fileLogger = std::make_unique<FileLogger>();
    if (!fileLogger->open((d.services.userDataDir / "logs").string(), 10)) {
        std::fprintf(stderr, "fighters-legacy: cannot open log file in %s, falling back to stderr\n",
                     (d.services.userDataDir / "logs").string().c_str());
    }
    d.services.rawLogger = fileLogger.get();
    d.services.p.logger = std::move(fileLogger);

    const char* baseRaw = SDL_GetBasePath();
    d.services.assetsRoot = baseRaw ? fs::path(baseRaw) : fs::path(".");
    d.services.p.filesystem = std::make_unique<SDL3Filesystem>(d.services.assetsRoot, d.services.userDataDir);

    d.services.userConfig.emplace(*d.services.p.filesystem, *d.services.rawLogger);
    d.services.userConfig->load();

    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--log-level") == 0)
            d.services.rawLogger->setMinLevel(parseLogLevel(argv[i + 1]));
        else if (std::strcmp(argv[i], "--connect") == 0)
            parseConnectArg(argv[i + 1], d.services.connectHost, d.services.connectPort);
        else if (std::strcmp(argv[i], "--operator-password") == 0)
            d.services.operatorPassword = argv[i + 1];
    }

    // Merge operator password: CLI arg > FL_OPERATOR_PASSWORD env var > [client].operator_password.
    // SDL_getenv is cross-platform (wraps GetEnvironmentVariableA on Windows).
    if (d.services.operatorPassword.empty()) {
        if (const char* ev = SDL_getenv("FL_OPERATOR_PASSWORD"); ev && *ev)
            d.services.operatorPassword = ev;
    }
    if (d.services.operatorPassword.empty())
        d.services.operatorPassword = d.services.userConfig->client().operatorPassword;

    auto oalAudio = std::make_unique<OALAudio>();
    if (!oalAudio->init()) {
        d.services.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, oalAudio->getLastError());
        return false;
    }
    d.services.p.audio = std::move(oalAudio);

    d.services.p.input = std::make_unique<SDL3Input>();
    d.services.p.joystick = std::make_unique<SDL3Joystick>();

    return true;
}

// Steps 8–14: window, crash reporter, renderer, async filesystem, graphics settings.
bool Game::initWindowAndRenderer() {
    auto& d = *m_impl;

    d.services.p.window = std::make_unique<SDL3Window>();

    CrashReporter::checkPreviousCrash(d.services.userDataDir.string(), d.services.p.window.get(), d.services.rawLogger,
                                      "https://github.com/jomkz/fighters-legacy/issues/new");

    d.services.crashInfo.engineVersion = FL_VERSION_STRING;
    d.services.crashInfo.populateOS();
    d.services.crashReporter.init({d.services.userDataDir.string(),
                                   "https://github.com/jomkz/fighters-legacy/issues/new", d.services.rawLogger,
                                   d.services.p.window.get()},
                                  d.services.crashInfo);
    d.services.crashReporterReady = true;

    auto* sdlWindow = static_cast<SDL3Window*>(d.services.p.window.get());
    sdlWindow->setInputSink(static_cast<SDL3Input*>(d.services.p.input.get()));
    sdlWindow->setJoystickSink(static_cast<SDL3Joystick*>(d.services.p.joystick.get()));

    if (!d.services.p.window->init("Fighters Legacy", 1280, 720)) {
        d.services.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "window init failed");
        return false;
    }

    d.services.p.display = std::make_unique<SDL3Display>();
    d.services.p.cursor = std::make_unique<SDL3Cursor>();

    d.services.p.renderer = createVulkanRenderer();
    if (!d.services.p.renderer->init(d.services.p.window.get())) {
        d.services.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "renderer init failed");
        return false;
    }

    auto asyncFs = std::make_unique<SDL3AsyncFilesystem>(d.services.assetsRoot, d.services.userDataDir);
    if (!asyncFs->init()) {
        d.services.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, asyncFs->getLastError());
        return false;
    }
    d.services.p.asyncFilesystem = std::move(asyncFs);

    d.services.resizeHandler.r = d.services.p.renderer.get();
    d.services.p.window->setEventHandler(&d.services.resizeHandler);
    d.services.crashReporter.setGpuInfo(d.services.p.renderer->gpuInfo());

    d.services.rendererSettings = buildRendererSettings(d.services.userConfig->graphics());
    d.services.p.renderer->applySettings(d.services.rendererSettings);

    return true;
}

// Steps 15–16: mod loading, asset manager, first-run routing.
bool Game::initContent() {
    auto& d = *m_impl;

    ModLoader modLoader(*d.services.p.filesystem, *d.services.rawLogger);
    auto packs = modLoader.load();
    const bool hasPacks = !packs.empty();

    CrashInfo::ModEntry modEntries[CrashInfo::kMaxMods];
    int modCount = 0;
    for (const auto& pack : packs) {
        if (modCount >= CrashInfo::kMaxMods)
            break;
        auto& e = modEntries[modCount++];
        std::snprintf(e.id, sizeof(e.id), "%s", pack->id());
        std::snprintf(e.version, sizeof(e.version), "%s", pack->version());
    }
    d.services.crashReporter.setMods(modEntries, modCount);

    d.services.assets = std::make_unique<AssetManager>(std::move(packs), *d.services.rawLogger);
    d.services.assets->initialize(d.services.p.window.get());

    FirstRun firstRun(*d.services.userConfig, *d.services.rawLogger);
    d.services.outcome = firstRun.check(hasPacks);

    return true;
}

// Steps 17–17d: entity registry, scene renderer, particle system, terrain, audio systems, sandbox.
void Game::initGameSystems() {
    auto& d = *m_impl;

    registerBuiltinParticlePresets(d.services.particleSystem);

    d.services.sceneRenderer = std::make_unique<fl::SceneRenderer>(
        d.services.renderBridge,
        [&reg = d.services.entityRegistry](uint32_t idx, std::string& mesh, std::string& dmg) -> bool {
            const fl::EntityDef* def = reg.byIndex(idx);
            if (!def)
                return false;
            mesh = def->mesh;
            dmg = def->classicDamageMesh;
            return true;
        },
        *d.services.assets, *d.services.p.renderer);
    d.services.sceneRenderer->setDrawDistance(d.services.rendererSettings.drawDistanceKm);
    d.services.sceneRenderer->setLogger(d.services.rawLogger);

    d.services.terrainStreamer =
        std::make_unique<fl::TerrainStreamer>(fl::builtinWorldTerrainManifest(), *d.services.assets,
                                              *d.services.p.asyncFilesystem, d.services.p.renderer.get());
    d.services.sceneRenderer->setTerrainStreamer(d.services.terrainStreamer.get());

    d.services.sceneRenderer->setParticleSystem(
        &d.services.particleSystem,
        [&reg = d.services.entityRegistry](uint32_t idx, uint8_t damageLevel) -> std::string {
            const fl::EntityDef* def = reg.byIndex(idx);
            if (!def || !def->damage)
                return {};
            const fl::DamagePenalty* pen = nullptr;
            switch (static_cast<fl::DamageLevel>(damageLevel)) {
            case fl::DamageLevel::Light:
                pen = &def->damage->light;
                break;
            case fl::DamageLevel::Heavy:
                pen = &def->damage->heavy;
                break;
            case fl::DamageLevel::Critical:
                pen = &def->damage->critical;
                break;
            default:
                break;
            }
            return pen ? pen->visualEffect : std::string{};
        });

    d.services.subtitleQueue.setEnabled(d.services.userConfig->accessibility().subtitlesEnabled);
    d.services.sceneRenderer->setSubtitleQueue(&d.services.subtitleQueue);

    if (d.services.musicManager.init(d.services.p.audio.get(), d.services.assets.get(), d.services.rawLogger)) {
        auto playlistText = d.services.assets->loadConfig("playlist.toml");
        PlaylistData playlist = parsePlaylist(playlistText.value_or(""), *d.services.rawLogger);
        d.services.musicManager.loadPlaylist(playlist);
        d.services.musicManager.setState(GameState::Menu);
    }
}

// Steps 19–20: debug console — console widget only; server commands wired in startGame().
void Game::initGameConsole() {
    auto& d = *m_impl;
    d.services.gameConsole.emplace(*d.services.rawLogger, d.services.cmdRegistry);
}

// Step 21: screen manager — created after all stable game systems exist.
void Game::initScreenManager() {
    auto& d = *m_impl;
    d.services.screenMgr = std::make_unique<ScreenManager>(*d.services.p.input, *d.services.rawLogger);
    d.services.screenMgr->init(*d.services.userConfig, *d.services.p.renderer, *d.services.p.window,
                               *d.services.p.display, *d.services.assets, !d.services.connectHost.empty());
}

// ---------------------------------------------------------------------------
// Session lifecycle — startGame / stopGame
// ---------------------------------------------------------------------------

void Game::startGame() {
    auto& d = *m_impl;

    // Reset render bridge and entity registry from any prior session.
    d.services.renderBridge.reset();
    d.services.entityRegistry.clear();
    d.services.env = EnvironmentState{};
    d.session.serverReady.store(false, std::memory_order_relaxed);
    d.session.sessionFailure.store(SessionFailure::None, std::memory_order_relaxed);

    // Register the builtin entity type for the no-pack sandbox path.
    if (d.services.outcome == FirstRunOutcome::LaunchSandboxInspector) {
        fl::EntityDef debugDef;
        debugDef.id = "builtin:debug-entity";
        debugDef.name = "Debug Entity";
        debugDef.category = fl::ObjectCategory::AirVehicle;
        debugDef.maxHp = 100.0f;
        d.services.entityRegistry.registerType(std::move(debugDef));
        d.services.cameraController.setFreeOrbit({0.0, 2000.0, 0.0}, 0.0f, 30.0f, 30.0f);
    }

    const bool isMultiplayer = !d.services.connectHost.empty();

    if (!isMultiplayer) {
        // Single-player: spawn fl-server subprocess in a background thread.
        d.session.localServer.emplace(*d.services.rawLogger);
        d.session.serverThread = std::thread([&d]() {
            auto result = d.session.localServer->start();
            if (result == LocalServer::StartResult::Ok) {
                d.session.serverReady.store(true, std::memory_order_release);
            } else {
                SessionFailure f = SessionFailure::None;
                switch (result) {
                case LocalServer::StartResult::SpawnFailed:
                    f = SessionFailure::ServerSpawnFailed;
                    break;
                case LocalServer::StartResult::BindFailed:
                    f = SessionFailure::ServerBindFailed;
                    break;
                case LocalServer::StartResult::Timeout:
                    f = SessionFailure::ServerStartTimeout;
                    break;
                case LocalServer::StartResult::Ok:
                    break; // handled above; silences -Wswitch
                }
                if (f != SessionFailure::None) {
                    SessionFailure expected = SessionFailure::None;
                    d.session.sessionFailure.compare_exchange_strong(expected, f, std::memory_order_release,
                                                                     std::memory_order_relaxed);
                }
            }
        });
    } else {
        // Multiplayer: no local server — signal ready immediately so LoadingScreen
        // skips the StartingServer phase on its first update().
        d.session.serverReady.store(true, std::memory_order_relaxed);
    }

    // onConnect is called by LoadingScreen once serverReady fires.
    auto onConnect = [&d, isMultiplayer]() {
        d.services.activeHud = &d.services.flightHud;
        d.session.hapticController.emplace(*d.services.p.input);

        d.session.clientNet = std::make_unique<ENetNetwork>();
        if (!d.session.clientNet->init()) {
            d.services.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "client ENet init failed");
            return;
        }

        d.session.clientHandler =
            std::make_unique<ClientNetEventHandler>(d.services.renderBridge, d.services.entityRegistry,
                                                    *d.services.rawLogger, *d.session.clientNet, d.services.env);
        d.session.clientHandler->notice = &d.services.serverNotice;
        d.session.clientHandler->console = &*d.services.gameConsole;
        d.session.clientHandler->motdDisplaySeconds = d.services.userConfig->client().motdDisplayS;
        d.session.clientHandler->sessionFailure = &d.session.sessionFailure;
        d.session.clientNet->setEventHandler(d.session.clientHandler.get());

        if (!isMultiplayer) {
            d.services.env = d.session.localServer->initialEnvironment();

            auto adminSender =
                makeNetworkAdminSender(*d.session.clientNet, std::string(d.session.localServer->sessionToken()));
            d.session.localServer->registerConsoleCommands(
                d.services.cmdRegistry, adminSender, d.services.renderBridge, &d.services.entityRegistry,
                &d.session.clientHandler->assignedEntityIdx, &d.session.clientHandler->assignedEntityGen,
                &d.services.gameConsole->showPosRef());
            d.services.screenMgr->setServerCmd(std::move(adminSender));

            d.session.discoveryListener.emplace(static_cast<uint16_t>(4778), *d.services.rawLogger);
            if (!d.session.discoveryListener->isOpen())
                d.services.rawLogger->log(LogLevel::Warn, __FILE__, __LINE__,
                                          "LAN discovery listener: no sockets opened");
        } else {
            // Multiplayer: wire admin commands if an operator password is available.
            CommandContext ctx{};
            ctx.renderBridge = &d.services.renderBridge;
            ctx.typeRegistry = &d.services.entityRegistry;
            ctx.playerEntityIdx = &d.session.clientHandler->assignedEntityIdx;
            ctx.playerEntityGen = &d.session.clientHandler->assignedEntityGen;
            ctx.showPos = &d.services.gameConsole->showPosRef();
            if (!d.services.operatorPassword.empty()) {
                auto adminSender = makeNetworkAdminSender(*d.session.clientNet, d.services.operatorPassword);
                ctx.serverCommand = adminSender;
                registerConsoleCommands(d.services.cmdRegistry, ctx);
                d.services.screenMgr->setServerCmd(std::move(adminSender));
            } else {
                registerConsoleCommands(d.services.cmdRegistry, ctx);
            }
        }

        // Build FlightScreenDeps now that all session objects exist.
        FlightScreenDeps fsd;
        fsd.camInput = &d.services.camInput;
        fsd.flightInput = &d.services.flightInput;
        fsd.cameraController = &d.services.cameraController;
        fsd.gameConsole = &*d.services.gameConsole;
        fsd.hapticController = &*d.session.hapticController;
        fsd.activeHud = &d.services.activeHud;
        fsd.windshieldRain = &d.services.windshieldRain;
        fsd.renderBridge = &d.services.renderBridge;
        fsd.terrainStreamer = d.services.terrainStreamer.get();
        fsd.env = &d.services.env;
        fsd.clientNet = d.session.clientNet.get();
        fsd.joystick = d.services.p.joystick.get();
        fsd.userConfig = &*d.services.userConfig;
        fsd.inspector = d.session.inspector ? &*d.session.inspector : nullptr;
        fsd.assignedEntityIdx = &d.session.clientHandler->assignedEntityIdx;
        fsd.assignedEntityGen = &d.session.clientHandler->assignedEntityGen;
        d.services.screenMgr->reinitFlight(std::move(fsd));

        const char* host = isMultiplayer ? d.services.connectHost.c_str() : "127.0.0.1";
        const uint16_t port = isMultiplayer ? d.services.connectPort : uint16_t{4778};
        d.session.clientNet->connect(host, port);
    };

    d.services.screenMgr->reinitLoading(
        d.session.serverReady, [&d]() { return d.services.renderBridge.hasSnapshot(); }, std::move(onConnect),
        !isMultiplayer, &d.session.sessionFailure);

    // Lazy SandboxInspector init (no-pack path).
    if (d.services.outcome == FirstRunOutcome::LaunchSandboxInspector)
        d.session.inspector.emplace(*d.services.p.audio, *d.services.p.input, *d.services.rawLogger, 440.0f, nullptr);
}

void Game::stopGame() {
    auto& d = *m_impl;

    // Join background server thread before touching any session objects.
    if (d.session.serverThread.joinable())
        d.session.serverThread.join();

    if (d.session.hapticController)
        d.session.hapticController->onPause(0);

    if (d.session.clientNet) {
        d.session.clientNet->disconnect();
        for (int i = 0; i < 10; ++i)
            d.session.clientNet->service(0);
        d.session.clientNet->shutdown();
        d.session.clientNet.reset();
    }
    if (d.session.localServer) {
        d.session.localServer->stop();
        d.session.localServer.reset();
    }

    d.session.clientHandler.reset();
    d.session.discoveryListener.reset();
    d.session.inspector.reset();
    d.session.hapticController.reset();
    d.services.renderBridge.reset();
    d.services.entityRegistry.clear();
    d.services.env = EnvironmentState{};
    d.services.musicManager.setState(GameState::Menu);
    d.services.screenMgr->setServerCmd(nullptr);
    d.services.p.input->setMouseCapture(false);
}

void Game::handleTransition(Screen next) {
    auto& d = *m_impl;
    const Screen prev = d.services.screenMgr->current();

    if (next == Screen::Loading && prev == Screen::MainMenu)
        startGame();

    if (next == Screen::MainMenu &&
        (prev == Screen::Flight || prev == Screen::Pause || prev == Screen::Debrief || prev == Screen::Loading))
        stopGame();

    if (next == Screen::Flight) {
        d.services.musicManager.setState(GameState::FlightPatrol);
        if (d.session.clientHandler && d.services.terrainStreamer) {
            const double radiusM = static_cast<double>(d.session.clientHandler->planetRadiusKm()) * 1000.0;
            d.services.terrainStreamer->setSphericalPlanetRadius(radiusM);
        }
    } else if (next == Screen::MainMenu)
        d.services.musicManager.setState(GameState::Menu);
    else if (next == Screen::Debrief) {
        d.services.screenMgr->debrief().setStats(0, 0, true);
        d.services.musicManager.setState(GameState::Debrief);
    }

    d.services.screenMgr->transition(next);
}

// ---------------------------------------------------------------------------
// Game loop
// ---------------------------------------------------------------------------

void Game::run() {
    auto& d = *m_impl;
    bool wasFocused = true;
    bool running = true;

    while (running && !d.services.p.window->shouldClose()) {
        d.services.p.window->pollEvents();

        // Haptic: pause effects on focus loss.
        {
            const bool isFocused = (SDL_GetWindowFlags(static_cast<SDL_Window*>(d.services.p.window->nativeHandle())) &
                                    SDL_WINDOW_INPUT_FOCUS) != 0;
            if (wasFocused && !isFocused && d.session.hapticController)
                d.session.hapticController->onPause(0);
            wasFocused = isFocused;
        }

        d.services.p.renderer->beginFrame();

        const Screen cur = d.services.screenMgr->current();
        const bool inSession =
            (cur == Screen::Flight || cur == Screen::Pause || cur == Screen::Debrief || cur == Screen::Loading);

        // Network service (session only).
        if (inSession && d.session.clientNet)
            d.session.clientNet->service(0);
        if (inSession && d.session.discoveryListener)
            d.session.discoveryListener->poll();

        // Render pipeline (session with valid snapshot only).
        CameraView cam{};
        glm::dvec3 camOrigin{};
        const fl::EntityRenderEntry* playerEntry = nullptr;
        if (inSession && d.session.clientHandler && d.services.renderBridge.hasSnapshot()) {
            playerEntry = findPlayerEntry(d.services.renderBridge, d.session.clientHandler->assignedEntityIdx,
                                          d.session.clientHandler->assignedEntityGen);
            const float alpha = d.session.clientHandler->tickAlpha.get();
            const float aspect =
                static_cast<float>(d.services.p.window->width()) /
                static_cast<float>(d.services.p.window->height() > 0 ? d.services.p.window->height() : 1);
            cam = d.services.cameraController.view(aspect);
            camOrigin = cam.worldOrigin;

            d.services.p.asyncFilesystem->service();
            d.services.terrainStreamer->update(camOrigin);
            updateAudioListener(*d.services.p.audio, cam, playerEntry ? playerEntry->velocity : glm::vec3{});

            const bool isSnow = static_cast<float>(camOrigin.y) > kSnowAltitudeThresholdM;
            d.services.sceneRenderer->renderFrame(
                alpha, cam, d.services.env,
                d.services.precipController.build(d.services.env, cam, isSnow, d.services.particleSystem));
        }

        // Audio update (always — so music plays on main menu too).
        {
            const AudioSettings& aud = d.services.userConfig->audio();
            d.services.subtitleQueue.update(1.0f / 60.0f);
            d.services.musicManager.update(1.0f / 60.0f, aud.masterVolume, aud.musicVolume);
        }

        // Screen update — hands off input/flight logic to the active screen.
        const Screen next = d.services.screenMgr->active().update(*d.services.p.input, *d.services.p.window);
        if (next == Screen::Quit) {
            if (inSession)
                stopGame();
            running = false;
        } else if (next != cur) {
            handleTransition(next);
        }

        // Console HUD (show position if we have a valid camera).
        d.services.gameConsole->buildHud(camOrigin != glm::dvec3{} ? &camOrigin : nullptr,
                                         playerEntry ? &playerEntry->position : nullptr);

        // Overlay layers: screen content + server notice + console.
        d.services.p.renderer->submitOverlayElements(d.services.screenMgr->active().buildElements());
        d.services.p.renderer->submitOverlayElements(d.services.serverNotice.buildElements());
        d.services.p.renderer->setConsoleElements(d.services.gameConsole->elements());

        updatePerfOverlay(*d.services.gameConsole, *d.services.p.renderer, d.services.perfOverlay,
                          d.services.renderBridge, *d.services.userConfig, cur == Screen::Flight);

        d.services.p.renderer->endFrame();
        d.services.p.input->flush();
        d.services.p.joystick->flush();
    }
}
