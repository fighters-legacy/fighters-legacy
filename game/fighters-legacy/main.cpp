// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#include "ENetNetwork.h"
#include "FileLogger.h"
#include "IWindowEventHandler.h"
#include "Platform.h"
#include "Version.h"
#include "audio/MusicManager.h"
#include "audio/PlaylistLoader.h"
#include "audio/SubtitleQueue.h"
#include "config/UserConfig.h"
#include "content/AssetManager.h"
#include "content/ModLoader.h"
#include "crash/CrashInfo.h"
#include "crash/CrashReporter.h"
#include "debug/DebugCommands.h"
#include "debug/DebugConsole.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "firstrun/FirstRun.h"
#include "loop/GameLoop.h"
#include "loop/GameState.h"
#include "net/DiscoveryListener.h"
#include "net/GameProtocol.h"
#include "net/WorldBroadcaster.h"
#include "openal/OALAudio.h"
#include "perf/PerformanceOverlay.h"
#include "render/BuiltinGeometry.h"
#include "render/CameraController.h"
#include "render/FlightHud.h"
#include "render/ParticleSystem.h"
#include "render/RenderSnapshot.h"
#include "render/SceneRenderer.h"
#include "render/SimRenderBridge.h"
#include "render/TerrainStreamer.h"
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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// ClientNetEventHandler — parses WorldSnapshot packets from the embedded server
// and posts them to the render bridge so SceneRenderer can display them.
// ---------------------------------------------------------------------------
struct ClientNetEventHandler : INetworkEventHandler {
    fl::SimRenderBridge& bridge;
    fl::EntityTypeRegistry& registry;
    ILogger& logger;
    INetwork& net;

    uint32_t assignedEntityIdx{0};
    uint32_t assignedEntityGen{0};

    ClientNetEventHandler(fl::SimRenderBridge& b, fl::EntityTypeRegistry& r, ILogger& l, INetwork& n)
        : bridge(b), registry(r), logger(l), net(n) {}

    void onConnect(uint32_t /*peerId*/) override {
        logger.log(LogLevel::Info, __FILE__, __LINE__, "connected to embedded server");
    }
    void onDisconnect(uint32_t /*peerId*/) override {
        logger.log(LogLevel::Info, __FILE__, __LINE__, "disconnected from embedded server");
    }
    void onReceive(uint32_t /*peerId*/, const void* data, std::size_t size) override {
        if (size < 1)
            return;
        const uint8_t msgId = *static_cast<const uint8_t*>(data);

        if (msgId == static_cast<uint8_t>(fl::MsgId::Hello)) {
            if (size < sizeof(fl::MsgHello))
                return;
            fl::MsgHello hello;
            std::memcpy(&hello, data, sizeof(hello));
            if (hello.protocolVersion != fl::kProtocolVersion) {
                logger.log(LogLevel::Error, __FILE__, __LINE__, "server protocol version mismatch — disconnecting");
                net.disconnect();
            }
            return;
        }

        if (msgId == static_cast<uint8_t>(fl::MsgId::ConnectAck)) {
            if (size < sizeof(fl::MsgConnectAck))
                return;
            fl::MsgConnectAck ack;
            std::memcpy(&ack, data, sizeof(ack));
            assignedEntityIdx = ack.assignedEntityIdx;
            assignedEntityGen = ack.assignedEntityGen;
            const uint8_t* typeData = static_cast<const uint8_t*>(data) + sizeof(ack);
            for (uint16_t i = 0; i < ack.typeCount; ++i) {
                if ((typeData - static_cast<const uint8_t*>(data)) + sizeof(fl::MsgEntityTypeDef) > size)
                    break;
                fl::MsgEntityTypeDef td;
                std::memcpy(&td, typeData, sizeof(td));
                typeData += sizeof(td);
                if (registry.findById(td.id))
                    continue; // already registered in single-player
                fl::EntityDef def;
                def.id = td.id;
                def.mesh = td.mesh;
                def.classicDamageMesh = td.dmgMesh;
                def.maxHp = 100.0f;
                registry.registerType(std::move(def));
            }
        } else if (msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot)) {
            if (size < sizeof(fl::MsgWorldSnapshotHeader))
                return;
            fl::MsgWorldSnapshotHeader hdr;
            std::memcpy(&hdr, data, sizeof(hdr));
            const std::size_t expected =
                sizeof(fl::MsgWorldSnapshotHeader) + hdr.entityCount * sizeof(fl::MsgEntityEntry);
            if (size < expected)
                return;

            fl::RenderSnapshot snap;
            snap.tickIndex = hdr.tickIndex;
            snap.entries.reserve(hdr.entityCount);

            const uint8_t* entryData = static_cast<const uint8_t*>(data) + sizeof(hdr);
            for (uint16_t i = 0; i < hdr.entityCount; ++i) {
                fl::MsgEntityEntry e;
                std::memcpy(&e, entryData + i * sizeof(e), sizeof(e));

                fl::EntityRenderEntry re;
                re.entityIdx = e.entityIdx;
                re.entityGen = e.entityGen;
                re.typeIndex = e.typeIndex;
                re.position = {e.pos[0], e.pos[1], e.pos[2]};
                re.velocity = {e.vel[0], e.vel[1], e.vel[2]};
                // Wire format: x,y,z,w — glm::quat constructor: (w,x,y,z)
                re.orientation = glm::quat(e.ori[3], e.ori[0], e.ori[1], e.ori[2]);
                re.damageLevel = e.damageLevel;
                re.playerOwned = (e.flags & 1u) != 0;
                re.throttle = e.throttle;
                re.fuelPct = e.fuelPct;
                snap.entries.push_back(re);
            }
            bridge.publishExternal(std::move(snap));
        }
    }
};

// Engine world origin = spawn point. Terrain grid is centred here (originX/Z = -7680).
// Y is a conservative pre-load altitude; TerrainStreamer::heightAt(0,0) replaces it
// once the first LOD-0 chunk is available (#173).
[[maybe_unused]] static constexpr glm::dvec3 kBuiltinSpawnPos{0.0, 570.0, 0.0};

int main(int argc, char** argv) {
    // Step 0: Early flag handling — before any platform init so they work
    // even when SDL/Vulkan are absent.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("fighters-legacy %s (%s)\n", FL_VERSION_STRING, FL_GIT_HASH);
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: fighters-legacy [--log-level debug|info|warn|error] [--version] [--help]\n");
            return 0;
        }
    }

    // Step 1: Resolve UserData directory.
    SDL_Init(0);
    char* prefRaw = SDL_GetPrefPath("jomkz", "fighters-legacy");
    fs::path userDataDir = prefRaw ? fs::path(prefRaw) : fs::path(".");
    if (prefRaw)
        SDL_free(prefRaw);

    Platform p;

    // Step 2: FileLogger — open at default level.
    auto fileLogger = std::make_unique<FileLogger>();
    if (!fileLogger->open((userDataDir / "logs").string(), 10)) {
        std::fprintf(stderr, "fighters-legacy: cannot open log file in %s, falling back to stderr\n",
                     (userDataDir / "logs").string().c_str());
    }
    FileLogger* rawLogger = fileLogger.get();
    p.logger = std::move(fileLogger);

    // Step 3: SDL3Filesystem — assets root is the directory containing the binary.
    const char* baseRaw = SDL_GetBasePath();
    fs::path assetsRoot = baseRaw ? fs::path(baseRaw) : fs::path(".");
    p.filesystem = std::make_unique<SDL3Filesystem>(assetsRoot, userDataDir);

    // Step 4: UserConfig — load persisted settings (missing file is non-fatal).
    UserConfig userConfig(*p.filesystem, *rawLogger);
    userConfig.load();

    // Step 5: Apply --log-level CLI override (session-only; does not persist).
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--log-level") == 0)
            rawLogger->setMinLevel(parseLogLevel(argv[i + 1]));
    }

    // Step 6: Audio backend.
    auto oalAudio = std::make_unique<OALAudio>();
    if (!oalAudio->init()) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, oalAudio->getLastError());
        return 1;
    }
    p.audio = std::move(oalAudio);

    // Step 7: Input and joystick backends — keep raw pointers before move for sink
    // wiring below.
    auto sdl3Input = std::make_unique<SDL3Input>();
    SDL3Input* rawInput = sdl3Input.get();
    p.input = std::move(sdl3Input);

    auto sdl3Joystick = std::make_unique<SDL3Joystick>();
    SDL3Joystick* rawJoystick = sdl3Joystick.get();
    p.joystick = std::move(sdl3Joystick);

    // Step 8: Window (SDL_ShowMessageBox safe before video init per SDL3 docs).
    auto window = std::make_unique<SDL3Window>();
    p.window = std::move(window);

    // Step 9: Post-crash dialog.
    CrashReporter::checkPreviousCrash(userDataDir.string(), p.window.get(), rawLogger,
                                      "https://github.com/jomkz/fighters-legacy/issues/new");

    // Step 10: CrashReporter — sentinel + signal handlers.
    CrashInfo crashInfo;
    crashInfo.engineVersion = FL_VERSION_STRING;
    crashInfo.populateOS();
    CrashReporter crashReporter;
    crashReporter.init(
        {userDataDir.string(), "https://github.com/jomkz/fighters-legacy/issues/new", rawLogger, p.window.get()},
        crashInfo);

    // Step 11: Platform init — wire input and joystick sinks before window init so
    // events are routed during the very first pollEvents() call.
    auto* sdlWindow = static_cast<SDL3Window*>(p.window.get());
    sdlWindow->setInputSink(rawInput);
    sdlWindow->setJoystickSink(rawJoystick);

    if (!p.window->init("Fighters Legacy", 1280, 720)) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "window init failed");
        crashReporter.shutdown();
        return 1;
    }

    // Step 11.5: Display and cursor — usable now that SDL_INIT_VIDEO is up.
    p.display = std::make_unique<SDL3Display>();
    p.cursor = std::make_unique<SDL3Cursor>();

    // Step 12: Renderer.
    auto renderer = createVulkanRenderer();
    p.renderer = std::move(renderer);
    if (!p.renderer->init(p.window.get())) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "renderer init failed");
        crashReporter.shutdown();
        return 1;
    }

    // Step 12.5: Async filesystem for terrain chunk streaming.
    auto asyncFs = std::make_unique<SDL3AsyncFilesystem>(assetsRoot, userDataDir);
    if (!asyncFs->init()) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, asyncFs->getLastError());
        crashReporter.shutdown();
        return 1;
    }
    p.asyncFilesystem = std::move(asyncFs);

    // Step 13: Resize handler — forwards window resize events to the renderer.
    struct ResizeHandler : IWindowEventHandler {
        IRenderer* r = nullptr;
        void onResize(int w, int h) override {
            r->onResize(w, h);
        }
        void onClose() override {}
    } resizeHandler;
    resizeHandler.r = p.renderer.get();
    p.window->setEventHandler(&resizeHandler);

    // Step 14: GPU info → crash reporter.
    crashReporter.setGpuInfo(p.renderer->gpuInfo());

    // Step 14.1: Translate GraphicsSettings → RendererSettings and apply to renderer.
    // drawDistanceKm is applied to sceneRenderer after it is constructed (step 17b.2).
    RendererSettings rendererSettings{};
    {
        const GraphicsSettings g = userConfig.graphics();
        switch (g.vsync) {
        case VsyncMode::Off:
            rendererSettings.vsync = RendererVsyncMode::Off;
            break;
        case VsyncMode::Adaptive:
            rendererSettings.vsync = RendererVsyncMode::Adaptive;
            break;
        default:
            rendererSettings.vsync = RendererVsyncMode::On;
            break;
        }
        rendererSettings.antiAliasing = g.antiAliasing;
        rendererSettings.bloom = (g.qualityPreset >= QualityLevel::Medium);
        switch (g.drawDistance) {
        case DrawDistance::Low:
            rendererSettings.drawDistanceKm = 15.0f;
            break;
        case DrawDistance::Medium:
            rendererSettings.drawDistanceKm = 30.0f;
            break;
        case DrawDistance::Ultra:
            rendererSettings.drawDistanceKm = 100.0f;
            break;
        default: // High
            rendererSettings.drawDistanceKm = 50.0f;
            break;
        }
        p.renderer->applySettings(rendererSettings);
    }

    // Step 15: Mod loading.
    ModLoader modLoader(*p.filesystem, *rawLogger);
    auto packs = modLoader.load();
    const bool hasPacks = !packs.empty();

    // Build crash reporter mod list before packs are moved into AssetManager.
    CrashInfo::ModEntry modEntries[CrashInfo::kMaxMods];
    int modCount = 0;
    for (const auto& pack : packs) {
        if (modCount >= CrashInfo::kMaxMods)
            break;
        auto& e = modEntries[modCount++];
        std::snprintf(e.id, sizeof(e.id), "%s", pack->id());
        std::snprintf(e.version, sizeof(e.version), "%s", pack->version());
    }
    crashReporter.setMods(modEntries, modCount);

    AssetManager assets(std::move(packs), *rawLogger);
    assets.initialize(p.window.get());

    // Step 16: First-run routing.
    FirstRun firstRun(userConfig, *rawLogger);
    auto outcome = firstRun.check(hasPacks);

    // Step 17a: Entity system — type registry and manager replace the Phase 2.1 NullSim stub.
    fl::EntityTypeRegistry entityRegistry;
    fl::EntityManager entityManager(*rawLogger, entityRegistry);

    // Step 17b: Render bridge — populated from network snapshots in client mode.
    // NOT wired to entityManager.setRenderBridge: WorldBroadcaster owns serialization.
    fl::SimRenderBridge renderBridge;

    // Step 17b.1: Particle system — preset registry + per-frame emitter accumulation.
    fl::ParticleSystem particleSystem;
    // Built-in presets covering the roadmap.md:68 acceptance criteria.
    particleSystem.registerPreset("explosion",
                                  {200.0f, 1.5f, 15.0f, {1.0f, 0.6f, 0.1f}, {0.4f, 0.2f, 0.1f}, 0.3f, 3.0f, true});
    particleSystem.registerPreset("fire",
                                  {120.0f, 2.0f, 8.0f, {1.0f, 0.4f, 0.05f}, {0.6f, 0.1f, 0.0f}, 0.2f, 1.5f, true});
    particleSystem.registerPreset("smoke",
                                  {60.0f, 4.0f, 3.0f, {0.4f, 0.4f, 0.4f}, {0.15f, 0.15f, 0.15f}, 0.5f, 3.0f, false});

    // Step 17b.2: Scene renderer — converts entity snapshots to FrameScene each frame.
    // MeshNameResolver breaks the circular dep between engine-render and engine-entity:
    // the lambda captures entityRegistry (main-thread-only, read-only after start()).
    fl::CameraController cameraController;
    fl::SceneRenderer sceneRenderer{renderBridge,
                                    [&entityRegistry](uint32_t idx, std::string& mesh, std::string& dmg) -> bool {
                                        const fl::EntityDef* def = entityRegistry.byIndex(idx);
                                        if (!def)
                                            return false;
                                        mesh = def->mesh;
                                        dmg = def->classicDamageMesh;
                                        return true;
                                    },
                                    assets, *p.renderer};

    // Apply draw distance from settings (must come after SceneRenderer construction).
    sceneRenderer.setDrawDistance(rendererSettings.drawDistanceKm);

    // Step 17b.3: Terrain streamer — async chunk lifecycle, LOD rings, height queries.
    fl::TerrainStreamer terrainStreamer(fl::builtinWorldTerrainManifest(), assets, *p.asyncFilesystem,
                                        p.renderer.get());
    sceneRenderer.setTerrainStreamer(&terrainStreamer);

    // Wire the particle system so damaged entities emit effects each frame.
    // EffectResolver uses the snapshot typeIndex + damageLevel without touching sim-thread state.
    sceneRenderer.setParticleSystem(&particleSystem,
                                    [&entityRegistry](uint32_t idx, uint8_t damageLevel) -> std::string {
                                        const fl::EntityDef* def = entityRegistry.byIndex(idx);
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

    // Step 17c.0: Audio subsystem — subtitle queue, music manager, initial game state.
    SubtitleQueue subtitleQueue;
    subtitleQueue.setEnabled(userConfig.accessibility().subtitlesEnabled);
    sceneRenderer.setSubtitleQueue(&subtitleQueue);

    MusicManager musicManager;
    if (musicManager.init(p.audio.get(), &assets, rawLogger)) {
        auto playlistText = assets.loadConfig("playlist.toml");
        PlaylistData playlist = parsePlaylist(playlistText.value_or(""), *rawLogger);
        musicManager.loadPlaylist(playlist);
        musicManager.setState(GameState::Menu);
    }

    // Step 17c: Sandbox mode — register builtin entity type, configure scene, spawn formation.
    // All entity registration and spawning happen before gameLoop.start() so no sim thread
    // exists yet; calling spawn() on the main thread is data-race-free in this window.
    if (outcome == FirstRunOutcome::LaunchSandboxInspector) {
        // Builtin debug type: mesh is intentionally empty so SceneRenderer falls back to the
        // tetrahedron palette, cycling 6 colors by entityIdx (3 opaque, 3 glass/transparent).
        fl::EntityDef debugDef;
        debugDef.id = "builtin:debug-entity";
        debugDef.name = "Debug Entity";
        debugDef.category = fl::ObjectCategory::AirVehicle;
        debugDef.maxHp = 100.0f;
        entityRegistry.registerType(std::move(debugDef));

        // Orbit the camera south of the formation (yaw=0 = south of pivot), looking north.
        cameraController.setFreeOrbit({0.0, 500.0, 0.0}, 0.0f, -10.0f, 200.0f);

        // Spawn 5 entities in a V-formation at 500 m altitude. Slots 0-2 are opaque
        // (red/green/blue), slots 3-4 are glass (yellow/purple) — exercises the transparent pass.
        const float kAlt = 500.0f;
        struct {
            float x, z;
        } kSlots[] = {{0, 0}, {-30, -25}, {30, -25}, {-60, -50}, {60, -50}};
        for (const auto& s : kSlots) {
            fl::EntityTransform t{};
            t.pos[0] = s.x;
            t.pos[1] = kAlt;
            t.pos[2] = s.z;
            entityManager.spawn("builtin:debug-entity", t);
        }
    }

    std::optional<SandboxInspector> inspector;
    if (outcome == FirstRunOutcome::LaunchSandboxInspector)
        inspector.emplace(*p.audio, *p.input, *rawLogger, 440.0f, &entityManager);

    // Step 17d: Embedded server — binds ENet on 127.0.0.1:4778, runs GameLoop + EntityManager.
    // WorldBroadcaster serializes entity state and broadcasts WorldSnapshot each tick.
    // ENet server host is owned exclusively by the server sim thread (thread-safe by ENet
    // per-host ownership contract).
    auto serverNet = std::make_unique<ENetNetwork>();
    if (!serverNet->init()) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "server ENet init failed");
        crashReporter.shutdown();
        return 1;
    }
    fl::WorldBroadcaster broadcaster(entityManager, entityRegistry, *serverNet, *rawLogger);
    serverNet->setEventHandler(&broadcaster);
    if (!serverNet->bind("127.0.0.1", 4778, /*maxClients=*/1)) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "server ENet bind failed on 127.0.0.1:4778");
        crashReporter.shutdown();
        return 1;
    }

    GameLoop serverLoop(broadcaster, *rawLogger);
    serverLoop.start();

    // Step 17e: Client — connects to embedded server on 127.0.0.1:4778.
    // Receives WorldSnapshot packets and feeds them into the render bridge.
    auto clientNet = std::make_unique<ENetNetwork>();
    if (!clientNet->init()) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "client ENet init failed");
        crashReporter.shutdown();
        return 1;
    }
    ClientNetEventHandler clientHandler(renderBridge, entityRegistry, *rawLogger, *clientNet);
    clientNet->setEventHandler(&clientHandler);
    clientNet->connect("127.0.0.1", 4778);

    // Step 17f: LAN server discovery listener — receives beacons from fl-server instances.
    // Populates a server list consumed by the server browser (issue #143). No UI yet.
    DiscoveryListener discoveryListener(4778, *rawLogger);
    if (!discoveryListener.isOpen())
        rawLogger->log(LogLevel::Warn, __FILE__, __LINE__, "LAN discovery listener: no sockets opened");

    // Step 18: Shell loop — main thread owns all HAL.
    // Angled sun (better PBR shading than the default straight-down direction).
    EnvironmentState env{};
    env.sunDirection = glm::normalize(glm::vec3{0.6f, 1.0f, 0.4f}); // sun above horizon

    // Sandbox demo: static fire emitter at world origin exercises the GPU particle pass
    // from frame 1 without requiring any entity damage state.  effectName points to a
    // string literal so the pointer is stable for the lifetime of the loop.
    static const ParticleEmitterState kDemoFire{
        {0.0f, 10.0f, 0.0f}, // position
        "fire",              // effectName
        1.5f,                // intensity
        80.0f,               // spawnRate
        2.0f,                // particleLifetime
        8.0f,                // initialSpeed
        {1.0f, 0.6f, 0.1f},  // colorStart
        {0.2f, 0.1f, 0.05f}, // colorEnd
        0.5f,                // sizeStart
        2.5f,                // sizeEnd
        true,                // additive (fire/explosion)
    };
    const std::span<const ParticleEmitterState> sandboxEmitters =
        inspector ? std::span<const ParticleEmitterState>{&kDemoFire, 1} : std::span<const ParticleEmitterState>{};

    // Sandbox free-look camera state — pivot is the formation centre at 500 m altitude.
    // Matches the initial setFreeOrbit call above (yaw=0, pitch=-10, dist=200).
    glm::dvec3 sbPivot{0.0, 500.0, 0.0}; // mutable — WASD/QE pan it
    float sbYaw = 0.0f;
    float sbPitch = -10.0f;
    float sbRadius = 200.0f;
    // Unified mouse tracking shared across all camera modes.
    float camLastMx{0.f}, camLastMy{0.f};
    bool camFirstFrame{true};

    // Chase orbit state (initialized when switching to Chase via F2).
    float chaseYaw{180.f}, chasePitch{20.f}, chaseRadius{25.f};

    // Cockpit look offset (accumulated from RMB drag; initialized on F1 switch).
    float cockpitYaw{0.f}, cockpitPitch{0.f};

    fl::FlightHud flightHud;

    // Performance overlay — initialise mode from persisted user config.
    PerformanceOverlay perfOverlay;
    perfOverlay.setMode(userConfig.debug().overlayMode);

    // Debug console — always available; backtick toggles it.
    DebugCommandRegistry dbgRegistry;
    DebugConsole dbgConsole(*rawLogger, dbgRegistry);
    registerBuiltinCommands(dbgRegistry,
                            {&entityManager, &entityRegistry, &renderBridge, &clientHandler.assignedEntityIdx,
                             &clientHandler.assignedEntityGen, &dbgConsole.showPosRef(), &serverLoop});

    bool running = true;
    while (running && !p.window->shouldClose()) {
        // Pull scroll events before pollEvents drains them — affects Free and Chase camera zoom.
        {
            SDL_PumpEvents();
            SDL_Event ev;
            while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_WHEEL, SDL_EVENT_MOUSE_WHEEL) > 0) {
                const float s = ev.wheel.y;
                if (cameraController.mode() == fl::CameraMode::Free)
                    sbRadius = std::clamp(sbRadius - s * 10.0f, 20.0f, 5000.0f);
                else if (cameraController.mode() == fl::CameraMode::Chase)
                    chaseRadius = std::clamp(chaseRadius - s * 2.0f, 5.0f, 200.0f);
            }
        }

        p.window->pollEvents();
        p.renderer->beginFrame();

        // Player entity lookup — uses the snapshot from the previous clientNet->service() call.
        // Stored as a raw pointer; valid until sceneRenderer.renderFrame() calls tryAdvance().
        const fl::EntityRenderEntry* playerEntry = nullptr;
        if (renderBridge.hasSnapshot()) {
            for (const auto& e : renderBridge.current().entries)
                if (e.entityIdx == clientHandler.assignedEntityIdx && e.entityGen == clientHandler.assignedEntityGen) {
                    playerEntry = &e;
                    break;
                }
        }

        // F1=Cockpit, F2=Chase, F4=Free — direct mode activation.
        // Backtick toggles the debug console.
        {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            static bool f1Prev{}, f2Prev{}, f4Prev{};
            static bool gravePrev{};
            bool graveNow = keys[SDL_SCANCODE_GRAVE] != 0;
            if (graveNow && !gravePrev) {
                if (dbgConsole.isOpen())
                    dbgConsole.close(*p.input);
                else
                    dbgConsole.open(*p.input);
            }
            gravePrev = graveNow;
            if (keys[SDL_SCANCODE_F1] && !f1Prev) {
                cameraController.setMode(fl::CameraMode::Cockpit);
                cockpitYaw = 0.f;
                cockpitPitch = 0.f;
                camFirstFrame = true;
            }
            if (keys[SDL_SCANCODE_F2] && !f2Prev) {
                cameraController.setMode(fl::CameraMode::Chase);
                if (playerEntry) {
                    float ey = std::atan2(2.f * (playerEntry->orientation.w * playerEntry->orientation.y +
                                                 playerEntry->orientation.x * playerEntry->orientation.z),
                                          1.f - 2.f * (playerEntry->orientation.y * playerEntry->orientation.y +
                                                       playerEntry->orientation.z * playerEntry->orientation.z));
                    chaseYaw = glm::degrees(ey) + 180.f;
                }
                chasePitch = 20.f;
                chaseRadius = 25.f;
                camFirstFrame = true;
            }
            if (keys[SDL_SCANCODE_F4] && !f4Prev) {
                cameraController.setMode(fl::CameraMode::Free);
                camFirstFrame = true;
            }
            f1Prev = keys[SDL_SCANCODE_F1];
            f2Prev = keys[SDL_SCANCODE_F2];
            f4Prev = keys[SDL_SCANCODE_F4];
        }

        // Per-mode camera update.
        {
            float mx = 0, my = 0;
            SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
            const bool* keys = SDL_GetKeyboardState(nullptr);

            switch (cameraController.mode()) {
            case fl::CameraMode::Free: {
                if (!camFirstFrame && (mb & SDL_BUTTON_LMASK)) {
                    sbYaw -= (mx - camLastMx) * 0.35f;
                    sbPitch += (my - camLastMy) * 0.25f;
                    sbPitch = std::clamp(sbPitch, -89.0f, 89.0f);
                }
                if (keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS])
                    sbRadius = std::max(20.0f, sbRadius - 5.0f);
                if (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS])
                    sbRadius = std::min(5000.0f, sbRadius + 5.0f);
                const float speed = std::max(1.0f, sbRadius * 0.01f);
                const float yr = glm::radians(sbYaw);
                const glm::vec3 fwd{-std::sin(yr), 0.0f, -std::cos(yr)};
                const glm::vec3 rgt{std::cos(yr), 0.0f, -std::sin(yr)};
                if (keys[SDL_SCANCODE_W])
                    sbPivot += glm::dvec3(fwd * speed);
                if (keys[SDL_SCANCODE_S])
                    sbPivot -= glm::dvec3(fwd * speed);
                if (keys[SDL_SCANCODE_D])
                    sbPivot += glm::dvec3(rgt * speed);
                if (keys[SDL_SCANCODE_A])
                    sbPivot -= glm::dvec3(rgt * speed);
                if (keys[SDL_SCANCODE_E])
                    sbPivot.y += speed;
                if (keys[SDL_SCANCODE_Q])
                    sbPivot.y -= speed;
                if (keys[SDL_SCANCODE_R]) {
                    sbPivot = {0.0, 500.0, 0.0};
                    sbYaw = 0.f;
                    sbPitch = -10.f;
                    sbRadius = 200.f;
                }
                cameraController.setFreeOrbit(sbPivot, sbYaw, sbPitch, sbRadius);
                break;
            }
            case fl::CameraMode::Chase:
                if (playerEntry) {
                    if (!camFirstFrame && (mb & SDL_BUTTON_LMASK)) {
                        chaseYaw -= (mx - camLastMx) * 0.35f;
                        chasePitch += (my - camLastMy) * 0.25f;
                        chasePitch = std::clamp(chasePitch, -89.0f, 89.0f);
                    }
                    cameraController.setFreeOrbit(playerEntry->position, chaseYaw, chasePitch, chaseRadius);
                }
                break;
            case fl::CameraMode::Cockpit:
                if (playerEntry) {
                    cameraController.setTarget(playerEntry->position, playerEntry->orientation);
                    if (!camFirstFrame && (mb & SDL_BUTTON_RMASK)) {
                        cockpitYaw -= (mx - camLastMx) * 0.35f;
                        cockpitPitch += (my - camLastMy) * 0.25f;
                        cockpitPitch = std::clamp(cockpitPitch, -80.0f, 80.0f);
                    }
                    cameraController.setCockpitLook(cockpitYaw, cockpitPitch);
                }
                break;
            }
            camLastMx = mx;
            camLastMy = my;
            camFirstFrame = false;
        }

        if (inspector && !inspector->update())
            running = false;

        // Pump ENet inbound (non-blocking). ClientNetEventHandler::onReceive fires here
        // for WorldSnapshot packets → publishExternal → renderBridge updated.
        clientNet->service(0);

        // Poll LAN discovery (non-blocking). Updates server list for issue #143 server browser.
        discoveryListener.poll();

        // Send client flight inputs to the embedded server each frame.
        // Arrow keys: Up/Down = elevator, Left/Right = aileron, Z/X = rudder.
        // Left Shift = full throttle; Space = weapon trigger.
        // Inputs are zeroed when the debug console is open.
        {
            static uint32_t inputSeq = 0;
            const bool* keys = SDL_GetKeyboardState(nullptr);
            fl::MsgClientInput inp;
            inp.seqNum = inputSeq++;
            inp.tickIndex = renderBridge.hasSnapshot() ? renderBridge.current().tickIndex : 0;
            if (!dbgConsole.isOpen()) {
                inp.throttle = keys[SDL_SCANCODE_LSHIFT] ? 1.f : 0.f;
                inp.elevator = (keys[SDL_SCANCODE_UP] ? -1.f : 0.f) + (keys[SDL_SCANCODE_DOWN] ? 1.f : 0.f);
                inp.aileron = (keys[SDL_SCANCODE_RIGHT] ? 1.f : 0.f) + (keys[SDL_SCANCODE_LEFT] ? -1.f : 0.f);
                inp.rudder = (keys[SDL_SCANCODE_X] ? 1.f : 0.f) + (keys[SDL_SCANCODE_Z] ? -1.f : 0.f);
                inp.buttons = keys[SDL_SCANCODE_SPACE] ? 1u : 0u; // bit 0 = weapon trigger
            }
            // peerId is ignored by ENetNetwork on a client; sends to the single connected peer (server).
            clientNet->send(0, &inp, sizeof(inp), /*reliable=*/true);
        }

        // Alpha from server loop's tick timing (same atomic read pattern as before).
        float alpha = serverLoop.shellTick();
        float aspect =
            static_cast<float>(p.window->width()) / static_cast<float>(p.window->height() > 0 ? p.window->height() : 1);
        CameraView cam = cameraController.view(aspect);

        // Service async I/O and advance terrain chunk lifecycle for this camera position.
        p.asyncFilesystem->service();
        terrainStreamer.update(cam.worldOrigin);

        // Update audio listener from camera pose each frame.
        // Column-major glm::mat4: view[col][row]. Column 2 = -forward (RH); column 1 = up.
        {
            glm::vec3 fwd = -glm::vec3(cam.view[2][0], cam.view[2][1], cam.view[2][2]);
            glm::vec3 up = glm::vec3(cam.view[1][0], cam.view[1][1], cam.view[1][2]);
            const float pos[3] = {static_cast<float>(cam.worldOrigin.x), static_cast<float>(cam.worldOrigin.y),
                                  static_cast<float>(cam.worldOrigin.z)};
            const float fwdA[3] = {fwd.x, fwd.y, fwd.z};
            const float upA[3] = {up.x, up.y, up.z};
            const float zero[3] = {};
            p.audio->setListenerTransform(pos, fwdA, upA);
            p.audio->setListenerVelocity(zero);
        }

        // Per-frame audio updates.
        const AudioSettings& aud = userConfig.audio();
        subtitleQueue.update(1.0f / 60.0f); // fixed timestep approximation; good enough for display
        musicManager.update(1.0f / 60.0f, aud.masterVolume, aud.musicVolume);

        sceneRenderer.renderFrame(alpha, cam, env, sandboxEmitters);

        // HUD: active only in Cockpit mode. playerEntry may be nullptr (no snapshot yet or
        // tryAdvance() updated the bridge, but the pointer remains valid this frame — see comment above).
        flightHud.update(cameraController.mode() == fl::CameraMode::Cockpit ? playerEntry : nullptr);

        // Debug console tick and HUD build.
        {
            if (dbgConsole.isOpen()) {
                if (dbgConsole.tick(*p.input))
                    dbgConsole.close(*p.input);
            }
            glm::dvec3 playerPos{};
            const glm::dvec3* playerPosPtr = nullptr;
            if (playerEntry) {
                playerPos = playerEntry->position;
                playerPosPtr = &playerPos;
            }
            dbgConsole.buildHud(playerPosPtr);
        }

        // Performance overlay (F3) and 2D HUD submit.
        {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            static bool f3PrevDown = false;
            if (keys[SDL_SCANCODE_F3] && !f3PrevDown) {
                perfOverlay.cycleMode();
                DebugSettings ds = userConfig.debug();
                ds.overlayMode = perfOverlay.mode();
                userConfig.setDebug(ds);
                userConfig.save();
            }
            f3PrevDown = keys[SDL_SCANCODE_F3];

            perfOverlay.update(p.renderer->getFrameStats(), entityManager.liveCount(), 1000.0f / 60.0f);
            p.renderer->setOverlayLines(perfOverlay.lines());

            // Merge flight HUD + debug console elements into a single span.
            auto flightElems = flightHud.elements();
            auto dbgElems = dbgConsole.elements();
            std::vector<HudElement> allHud(flightElems.begin(), flightElems.end());
            allHud.insert(allHud.end(), dbgElems.begin(), dbgElems.end());
            p.renderer->submitHudElements(allHud);
        }

        p.renderer->endFrame();
        p.input->flush();
        p.joystick->flush();
    }

    // Step 19: Clean shutdown.
    serverLoop.stop(); // join sim thread before any ENet teardown
    serverNet->disconnect();
    serverNet->shutdown();
    clientNet->disconnect();
    clientNet->shutdown();
    inspector.reset();
    musicManager.shutdown(); // must come before audio shutdown
    p.cursor.reset();        // destroy cursor while SDL video is still alive (before SDL_Quit)
    p.audio->shutdown();
    p.renderer->shutdown();
    p.window->shutdown();
    crashReporter.shutdown();
    return 0;
}
