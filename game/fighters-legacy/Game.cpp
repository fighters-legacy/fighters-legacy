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

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
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

struct GameImpl {
    // Platform declared first → destroyed last, ensuring rawLogger stays valid
    // throughout the destruction of all other members.
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
    std::optional<SandboxInspector> inspector;

    // Network + HUD
    std::optional<LocalServer> localServer;
    std::unique_ptr<ENetNetwork> clientNet;
    EnvironmentState env;
    fl::FlightHud flightHud;
    fl::IHud* activeHud{nullptr};
    fl::WindshieldRain windshieldRain;
    ServerNotice serverNotice;
    std::optional<HapticController> hapticController;
    std::unique_ptr<ClientNetEventHandler> clientHandler;
    std::optional<DiscoveryListener> discoveryListener;

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

    // Session lifecycle (startGame / stopGame)
    std::thread serverThread;
    std::atomic<bool> serverReady{false};
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
    if (d.serverThread.joinable() || d.clientNet || d.localServer)
        stopGame();
    d.musicManager.shutdown();
    d.p.cursor.reset();
    if (d.p.audio)
        d.p.audio->shutdown();
    if (d.p.renderer)
        d.p.renderer->shutdown();
    if (d.p.window)
        d.p.window->shutdown();
    if (d.crashReporterReady)
        d.crashReporter.shutdown();
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
    d.userDataDir = prefRaw ? fs::path(prefRaw) : fs::path(".");
    if (prefRaw)
        SDL_free(prefRaw);

    auto fileLogger = std::make_unique<FileLogger>();
    if (!fileLogger->open((d.userDataDir / "logs").string(), 10)) {
        std::fprintf(stderr, "fighters-legacy: cannot open log file in %s, falling back to stderr\n",
                     (d.userDataDir / "logs").string().c_str());
    }
    d.rawLogger = fileLogger.get();
    d.p.logger = std::move(fileLogger);

    const char* baseRaw = SDL_GetBasePath();
    d.assetsRoot = baseRaw ? fs::path(baseRaw) : fs::path(".");
    d.p.filesystem = std::make_unique<SDL3Filesystem>(d.assetsRoot, d.userDataDir);

    d.userConfig.emplace(*d.p.filesystem, *d.rawLogger);
    d.userConfig->load();

    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--log-level") == 0)
            d.rawLogger->setMinLevel(parseLogLevel(argv[i + 1]));
    }

    auto oalAudio = std::make_unique<OALAudio>();
    if (!oalAudio->init()) {
        d.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, oalAudio->getLastError());
        return false;
    }
    d.p.audio = std::move(oalAudio);

    d.p.input = std::make_unique<SDL3Input>();
    d.p.joystick = std::make_unique<SDL3Joystick>();

    return true;
}

// Steps 8–14: window, crash reporter, renderer, async filesystem, graphics settings.
bool Game::initWindowAndRenderer() {
    auto& d = *m_impl;

    d.p.window = std::make_unique<SDL3Window>();

    CrashReporter::checkPreviousCrash(d.userDataDir.string(), d.p.window.get(), d.rawLogger,
                                      "https://github.com/jomkz/fighters-legacy/issues/new");

    d.crashInfo.engineVersion = FL_VERSION_STRING;
    d.crashInfo.populateOS();
    d.crashReporter.init(
        {d.userDataDir.string(), "https://github.com/jomkz/fighters-legacy/issues/new", d.rawLogger, d.p.window.get()},
        d.crashInfo);
    d.crashReporterReady = true;

    auto* sdlWindow = static_cast<SDL3Window*>(d.p.window.get());
    sdlWindow->setInputSink(static_cast<SDL3Input*>(d.p.input.get()));
    sdlWindow->setJoystickSink(static_cast<SDL3Joystick*>(d.p.joystick.get()));

    if (!d.p.window->init("Fighters Legacy", 1280, 720)) {
        d.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "window init failed");
        return false;
    }

    d.p.display = std::make_unique<SDL3Display>();
    d.p.cursor = std::make_unique<SDL3Cursor>();

    d.p.renderer = createVulkanRenderer();
    if (!d.p.renderer->init(d.p.window.get())) {
        d.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "renderer init failed");
        return false;
    }

    auto asyncFs = std::make_unique<SDL3AsyncFilesystem>(d.assetsRoot, d.userDataDir);
    if (!asyncFs->init()) {
        d.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, asyncFs->getLastError());
        return false;
    }
    d.p.asyncFilesystem = std::move(asyncFs);

    d.resizeHandler.r = d.p.renderer.get();
    d.p.window->setEventHandler(&d.resizeHandler);
    d.crashReporter.setGpuInfo(d.p.renderer->gpuInfo());

    d.rendererSettings = buildRendererSettings(d.userConfig->graphics());
    d.p.renderer->applySettings(d.rendererSettings);

    return true;
}

// Steps 15–16: mod loading, asset manager, first-run routing.
bool Game::initContent() {
    auto& d = *m_impl;

    ModLoader modLoader(*d.p.filesystem, *d.rawLogger);
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
    d.crashReporter.setMods(modEntries, modCount);

    d.assets = std::make_unique<AssetManager>(std::move(packs), *d.rawLogger);
    d.assets->initialize(d.p.window.get());

    FirstRun firstRun(*d.userConfig, *d.rawLogger);
    d.outcome = firstRun.check(hasPacks);

    return true;
}

// Steps 17–17d: entity registry, scene renderer, particle system, terrain, audio systems, sandbox.
void Game::initGameSystems() {
    auto& d = *m_impl;

    registerBuiltinParticlePresets(d.particleSystem);

    d.sceneRenderer = std::make_unique<fl::SceneRenderer>(
        d.renderBridge,
        [&reg = d.entityRegistry](uint32_t idx, std::string& mesh, std::string& dmg) -> bool {
            const fl::EntityDef* def = reg.byIndex(idx);
            if (!def)
                return false;
            mesh = def->mesh;
            dmg = def->classicDamageMesh;
            return true;
        },
        *d.assets, *d.p.renderer);
    d.sceneRenderer->setDrawDistance(d.rendererSettings.drawDistanceKm);
    d.sceneRenderer->setLogger(d.rawLogger);

    d.terrainStreamer = std::make_unique<fl::TerrainStreamer>(fl::builtinWorldTerrainManifest(), *d.assets,
                                                              *d.p.asyncFilesystem, d.p.renderer.get());
    d.sceneRenderer->setTerrainStreamer(d.terrainStreamer.get());

    d.sceneRenderer->setParticleSystem(&d.particleSystem,
                                       [&reg = d.entityRegistry](uint32_t idx, uint8_t damageLevel) -> std::string {
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

    d.subtitleQueue.setEnabled(d.userConfig->accessibility().subtitlesEnabled);
    d.sceneRenderer->setSubtitleQueue(&d.subtitleQueue);

    if (d.musicManager.init(d.p.audio.get(), d.assets.get(), d.rawLogger)) {
        auto playlistText = d.assets->loadConfig("playlist.toml");
        PlaylistData playlist = parsePlaylist(playlistText.value_or(""), *d.rawLogger);
        d.musicManager.loadPlaylist(playlist);
        d.musicManager.setState(GameState::Menu);
    }
}

// Steps 19–20: debug console — console widget only; server commands wired in startGame().
void Game::initGameConsole() {
    auto& d = *m_impl;
    d.gameConsole.emplace(*d.rawLogger, d.cmdRegistry);
}

// Step 21: screen manager — created after all stable game systems exist.
void Game::initScreenManager() {
    auto& d = *m_impl;
    d.screenMgr = std::make_unique<ScreenManager>(*d.p.input, *d.rawLogger);
    d.screenMgr->init(*d.userConfig, *d.p.renderer, *d.p.window, *d.p.display, *d.assets);
}

// ---------------------------------------------------------------------------
// Session lifecycle — startGame / stopGame
// ---------------------------------------------------------------------------

void Game::startGame() {
    auto& d = *m_impl;

    // Reset render bridge and entity registry from any prior session.
    d.renderBridge.reset();
    d.entityRegistry.clear();
    d.env = EnvironmentState{};
    d.serverReady.store(false, std::memory_order_relaxed);

    // Register the builtin entity type for the no-pack sandbox path.
    if (d.outcome == FirstRunOutcome::LaunchSandboxInspector) {
        fl::EntityDef debugDef;
        debugDef.id = "builtin:debug-entity";
        debugDef.name = "Debug Entity";
        debugDef.category = fl::ObjectCategory::AirVehicle;
        debugDef.maxHp = 100.0f;
        d.entityRegistry.registerType(std::move(debugDef));
        d.cameraController.setFreeOrbit({0.0, 2000.0, 0.0}, 0.0f, 30.0f, 30.0f);
    }

    // Start fl-server in a background thread.
    d.localServer.emplace(*d.rawLogger);
    d.serverThread = std::thread([&d]() {
        if (d.localServer->start())
            d.serverReady.store(true, std::memory_order_release);
    });

    // onConnect is called by LoadingScreen once serverReady fires.
    auto onConnect = [&d]() {
        d.activeHud = &d.flightHud;
        d.hapticController.emplace(*d.p.input);

        d.clientNet = std::make_unique<ENetNetwork>();
        if (!d.clientNet->init()) {
            d.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "client ENet init failed");
            return;
        }

        d.env = d.localServer->initialEnvironment();
        d.clientHandler = std::make_unique<ClientNetEventHandler>(d.renderBridge, d.entityRegistry, *d.rawLogger,
                                                                  *d.clientNet, d.env);
        d.clientHandler->notice = &d.serverNotice;
        d.clientHandler->console = &*d.gameConsole;
        d.clientHandler->motdDisplaySeconds = d.userConfig->client().motdDisplayS;
        d.clientNet->setEventHandler(d.clientHandler.get());

        auto adminSender = makeNetworkAdminSender(*d.clientNet, std::string(d.localServer->sessionToken()));
        d.localServer->registerConsoleCommands(d.cmdRegistry, adminSender, d.renderBridge, &d.entityRegistry,
                                               &d.clientHandler->assignedEntityIdx, &d.clientHandler->assignedEntityGen,
                                               &d.gameConsole->showPosRef());
        d.screenMgr->setServerCmd(std::move(adminSender));

        d.discoveryListener.emplace(static_cast<uint16_t>(4778), *d.rawLogger);
        if (!d.discoveryListener->isOpen())
            d.rawLogger->log(LogLevel::Warn, __FILE__, __LINE__, "LAN discovery listener: no sockets opened");

        // Build FlightScreenDeps now that all session objects exist.
        FlightScreenDeps fsd;
        fsd.camInput = &d.camInput;
        fsd.flightInput = &d.flightInput;
        fsd.cameraController = &d.cameraController;
        fsd.gameConsole = &*d.gameConsole;
        fsd.hapticController = &*d.hapticController;
        fsd.activeHud = &d.activeHud;
        fsd.windshieldRain = &d.windshieldRain;
        fsd.renderBridge = &d.renderBridge;
        fsd.terrainStreamer = d.terrainStreamer.get();
        fsd.env = &d.env;
        fsd.clientNet = d.clientNet.get();
        fsd.joystick = d.p.joystick.get();
        fsd.userConfig = &*d.userConfig;
        fsd.inspector = d.inspector ? &*d.inspector : nullptr;
        fsd.assignedEntityIdx = &d.clientHandler->assignedEntityIdx;
        fsd.assignedEntityGen = &d.clientHandler->assignedEntityGen;
        d.screenMgr->reinitFlight(std::move(fsd));

        d.clientNet->connect("127.0.0.1", 4778);
    };

    d.screenMgr->reinitLoading(d.serverReady, [&d]() { return d.renderBridge.hasSnapshot(); }, std::move(onConnect));

    // Lazy SandboxInspector init (no-pack path).
    if (d.outcome == FirstRunOutcome::LaunchSandboxInspector)
        d.inspector.emplace(*d.p.audio, *d.p.input, *d.rawLogger, 440.0f, nullptr);
}

void Game::stopGame() {
    auto& d = *m_impl;

    // Join background server thread before touching any session objects.
    if (d.serverThread.joinable())
        d.serverThread.join();

    if (d.hapticController)
        d.hapticController->onPause(0);

    if (d.clientNet) {
        d.clientNet->disconnect();
        for (int i = 0; i < 10; ++i)
            d.clientNet->service(0);
        d.clientNet->shutdown();
        d.clientNet.reset();
    }
    if (d.localServer) {
        d.localServer->stop();
        d.localServer.reset();
    }

    d.clientHandler.reset();
    d.discoveryListener.reset();
    d.inspector.reset();
    d.hapticController.reset();
    d.renderBridge.reset();
    d.entityRegistry.clear();
    d.env = EnvironmentState{};
    d.musicManager.setState(GameState::Menu);
    d.screenMgr->setServerCmd(nullptr);
    d.p.input->setMouseCapture(false);
}

void Game::handleTransition(Screen next) {
    auto& d = *m_impl;
    const Screen prev = d.screenMgr->current();

    if (next == Screen::Loading && prev == Screen::MainMenu)
        startGame();

    if (next == Screen::MainMenu &&
        (prev == Screen::Flight || prev == Screen::Pause || prev == Screen::Debrief || prev == Screen::Loading))
        stopGame();

    if (next == Screen::Flight)
        d.musicManager.setState(GameState::FlightPatrol);
    else if (next == Screen::MainMenu)
        d.musicManager.setState(GameState::Menu);
    else if (next == Screen::Debrief) {
        d.screenMgr->debrief().setStats(0, 0, true);
        d.musicManager.setState(GameState::Debrief);
    }

    d.screenMgr->transition(next);
}

// ---------------------------------------------------------------------------
// Game loop
// ---------------------------------------------------------------------------

void Game::run() {
    auto& d = *m_impl;
    bool wasFocused = true;
    bool running = true;

    while (running && !d.p.window->shouldClose()) {
        d.p.window->pollEvents();

        // Haptic: pause effects on focus loss.
        {
            const bool isFocused = (SDL_GetWindowFlags(static_cast<SDL_Window*>(d.p.window->nativeHandle())) &
                                    SDL_WINDOW_INPUT_FOCUS) != 0;
            if (wasFocused && !isFocused && d.hapticController)
                d.hapticController->onPause(0);
            wasFocused = isFocused;
        }

        d.p.renderer->beginFrame();

        const Screen cur = d.screenMgr->current();
        const bool inSession =
            (cur == Screen::Flight || cur == Screen::Pause || cur == Screen::Debrief || cur == Screen::Loading);

        // Network service (session only).
        if (inSession && d.clientNet)
            d.clientNet->service(0);
        if (inSession && d.discoveryListener)
            d.discoveryListener->poll();

        // Render pipeline (session with valid snapshot only).
        CameraView cam{};
        glm::dvec3 camOrigin{};
        const fl::EntityRenderEntry* playerEntry = nullptr;
        if (inSession && d.clientHandler && d.renderBridge.hasSnapshot()) {
            playerEntry =
                findPlayerEntry(d.renderBridge, d.clientHandler->assignedEntityIdx, d.clientHandler->assignedEntityGen);
            const float alpha = d.clientHandler->tickAlpha.get();
            const float aspect = static_cast<float>(d.p.window->width()) /
                                 static_cast<float>(d.p.window->height() > 0 ? d.p.window->height() : 1);
            cam = d.cameraController.view(aspect);
            camOrigin = cam.worldOrigin;

            d.p.asyncFilesystem->service();
            d.terrainStreamer->update(camOrigin);
            updateAudioListener(*d.p.audio, cam, playerEntry ? playerEntry->velocity : glm::vec3{});

            const bool isSnow = static_cast<float>(camOrigin.y) > kSnowAltitudeThresholdM;
            d.sceneRenderer->renderFrame(alpha, cam, d.env,
                                         d.precipController.build(d.env, cam, isSnow, d.particleSystem));
        }

        // Audio update (always — so music plays on main menu too).
        {
            const AudioSettings& aud = d.userConfig->audio();
            d.subtitleQueue.update(1.0f / 60.0f);
            d.musicManager.update(1.0f / 60.0f, aud.masterVolume, aud.musicVolume);
        }

        // Screen update — hands off input/flight logic to the active screen.
        const Screen next = d.screenMgr->active().update(*d.p.input, *d.p.window);
        if (next == Screen::Quit) {
            if (inSession)
                stopGame();
            running = false;
        } else if (next != cur) {
            handleTransition(next);
        }

        // Console HUD (show position if we have a valid camera).
        d.gameConsole->buildHud(camOrigin != glm::dvec3{} ? &camOrigin : nullptr,
                                playerEntry ? &playerEntry->position : nullptr);

        // Overlay layers: screen content + server notice + console.
        d.p.renderer->submitOverlayElements(d.screenMgr->active().buildElements());
        d.p.renderer->submitOverlayElements(d.serverNotice.buildElements());
        d.p.renderer->setConsoleElements(d.gameConsole->elements());

        updatePerfOverlay(*d.gameConsole, *d.p.renderer, d.perfOverlay, d.renderBridge, *d.userConfig,
                          cur == Screen::Flight);

        d.p.renderer->endFrame();
        d.p.input->flush();
        d.p.joystick->flush();
    }
}
