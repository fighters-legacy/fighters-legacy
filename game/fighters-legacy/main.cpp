// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#include "CameraInput.h"
#include "ClientNetEventHandler.h"
#include "ENetNetwork.h"
#include "FileLogger.h"
#include "GameHud.h"
#include "IWindowEventHandler.h"
#include "LocalServer.h"
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
#include "debug/DebugCommandRegistry.h"
#include "debug/DebugConsole.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
#include "firstrun/FirstRun.h"
#include "net/DiscoveryListener.h"
#include "net/GameProtocol.h"
#include "openal/OALAudio.h"
#include "perf/PerformanceOverlay.h"
#include "render/BuiltinGeometry.h"
#include "render/CameraController.h"
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
// Helpers
// ---------------------------------------------------------------------------

// Returns a serverCommand function that sends MsgAdminCommand over a client ENet connection.
// Used by both single-player (session token from LocalServer) and future multiplayer.
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

static void registerBuiltinParticlePresets(fl::ParticleSystem& ps) {
    ps.registerPreset("explosion", {200.0f, 1.5f, 15.0f, {1.0f, 0.6f, 0.1f}, {0.4f, 0.2f, 0.1f}, 0.3f, 3.0f, true});
    ps.registerPreset("fire", {120.0f, 2.0f, 8.0f, {1.0f, 0.4f, 0.05f}, {0.6f, 0.1f, 0.0f}, 0.2f, 1.5f, true});
    ps.registerPreset("smoke", {60.0f, 4.0f, 3.0f, {0.4f, 0.4f, 0.4f}, {0.15f, 0.15f, 0.15f}, 0.5f, 3.0f, false});
    ps.registerPreset(
        "rain",
        {600.0f, 1.5f, 40.0f, {0.5f, 0.6f, 0.8f}, {0.3f, 0.4f, 0.6f}, 0.05f, 0.05f, false, {0.0f, -1.0f, 0.0f}});
    ps.registerPreset(
        "storm_rain",
        {1200.0f, 1.2f, 50.0f, {0.6f, 0.7f, 0.9f}, {0.3f, 0.4f, 0.6f}, 0.08f, 0.08f, false, {0.0f, -1.0f, 0.0f}});
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // Step 0: Early flag handling — before any platform init.
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

    // Step 2: FileLogger.
    auto fileLogger = std::make_unique<FileLogger>();
    if (!fileLogger->open((userDataDir / "logs").string(), 10)) {
        std::fprintf(stderr, "fighters-legacy: cannot open log file in %s, falling back to stderr\n",
                     (userDataDir / "logs").string().c_str());
    }
    FileLogger* rawLogger = fileLogger.get();
    p.logger = std::move(fileLogger);

    // Step 3: SDL3Filesystem.
    const char* baseRaw = SDL_GetBasePath();
    fs::path assetsRoot = baseRaw ? fs::path(baseRaw) : fs::path(".");
    p.filesystem = std::make_unique<SDL3Filesystem>(assetsRoot, userDataDir);

    // Step 4: UserConfig.
    UserConfig userConfig(*p.filesystem, *rawLogger);
    userConfig.load();

    // Step 5: Apply --log-level CLI override.
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

    // Step 7: Input and joystick.
    auto sdl3Input = std::make_unique<SDL3Input>();
    SDL3Input* rawInput = sdl3Input.get();
    p.input = std::move(sdl3Input);

    auto sdl3Joystick = std::make_unique<SDL3Joystick>();
    SDL3Joystick* rawJoystick = sdl3Joystick.get();
    p.joystick = std::move(sdl3Joystick);

    // Step 8: Window.
    p.window = std::make_unique<SDL3Window>();

    // Step 9: Post-crash dialog.
    CrashReporter::checkPreviousCrash(userDataDir.string(), p.window.get(), rawLogger,
                                      "https://github.com/jomkz/fighters-legacy/issues/new");

    // Step 10: CrashReporter.
    CrashInfo crashInfo;
    crashInfo.engineVersion = FL_VERSION_STRING;
    crashInfo.populateOS();
    CrashReporter crashReporter;
    crashReporter.init(
        {userDataDir.string(), "https://github.com/jomkz/fighters-legacy/issues/new", rawLogger, p.window.get()},
        crashInfo);

    // Step 11: Platform init — wire input sinks before window init.
    auto* sdlWindow = static_cast<SDL3Window*>(p.window.get());
    sdlWindow->setInputSink(rawInput);
    sdlWindow->setJoystickSink(rawJoystick);

    if (!p.window->init("Fighters Legacy", 1280, 720)) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "window init failed");
        crashReporter.shutdown();
        return 1;
    }

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

    // Step 12.5: Async filesystem for terrain streaming.
    auto asyncFs = std::make_unique<SDL3AsyncFilesystem>(assetsRoot, userDataDir);
    if (!asyncFs->init()) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, asyncFs->getLastError());
        crashReporter.shutdown();
        return 1;
    }
    p.asyncFilesystem = std::move(asyncFs);

    // Step 13: Resize handler.
    struct ResizeHandler : IWindowEventHandler {
        IRenderer* r = nullptr;
        void onResize(int w, int h) override {
            r->onResize(w, h);
        }
        void onClose() override {}
    } resizeHandler;
    resizeHandler.r = p.renderer.get();
    p.window->setEventHandler(&resizeHandler);

    crashReporter.setGpuInfo(p.renderer->gpuInfo());

    // Step 14: Apply graphics settings.
    RendererSettings rendererSettings = buildRendererSettings(userConfig.graphics());
    p.renderer->applySettings(rendererSettings);

    // Step 15: Mod loading.
    ModLoader modLoader(*p.filesystem, *rawLogger);
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
    crashReporter.setMods(modEntries, modCount);

    AssetManager assets(std::move(packs), *rawLogger);
    assets.initialize(p.window.get());

    // Step 16: First-run routing.
    FirstRun firstRun(userConfig, *rawLogger);
    auto outcome = firstRun.check(hasPacks);

    // Step 17: Entity type registry + render bridge + scene renderer.
    fl::EntityTypeRegistry entityRegistry;
    fl::SimRenderBridge renderBridge;

    fl::ParticleSystem particleSystem;
    registerBuiltinParticlePresets(particleSystem);

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
    sceneRenderer.setDrawDistance(rendererSettings.drawDistanceKm);

    fl::TerrainStreamer terrainStreamer(fl::builtinWorldTerrainManifest(), assets, *p.asyncFilesystem,
                                        p.renderer.get());
    sceneRenderer.setTerrainStreamer(&terrainStreamer);

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

    // Step 17c: Audio.
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

    // Step 17d: Sandbox setup.
    if (outcome == FirstRunOutcome::LaunchSandboxInspector) {
        fl::EntityDef debugDef;
        debugDef.id = "builtin:debug-entity";
        debugDef.name = "Debug Entity";
        debugDef.category = fl::ObjectCategory::AirVehicle;
        debugDef.maxHp = 100.0f;
        entityRegistry.registerType(std::move(debugDef));

        cameraController.setFreeOrbit({0.0, 2000.0, 0.0}, 0.0f, -10.0f, 200.0f);
    }

    std::optional<SandboxInspector> inspector;
    if (outcome == FirstRunOutcome::LaunchSandboxInspector)
        inspector.emplace(*p.audio, *p.input, *rawLogger, 440.0f, nullptr);

    // Step 18: Start local fl-server subprocess and connect client.
    LocalServer localServer(*rawLogger);
    if (!localServer.start()) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__,
                       "failed to start local fl-server — is fl-server built alongside fighters-legacy?");
        crashReporter.shutdown();
        return 1;
    }

    auto clientNet = std::make_unique<ENetNetwork>();
    if (!clientNet->init()) {
        rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "client ENet init failed");
        crashReporter.shutdown();
        return 1;
    }

    EnvironmentState env = localServer.initialEnvironment();
    GameHud gameHud;
    ClientNetEventHandler clientHandler(renderBridge, entityRegistry, *rawLogger, *clientNet, env);
    clientHandler.hud = &gameHud;
    clientNet->setEventHandler(&clientHandler);
    clientNet->connect("127.0.0.1", 4778);

    // Step 18.5: LAN discovery listener (populates server browser, issue #143).
    DiscoveryListener discoveryListener(4778, *rawLogger);
    if (!discoveryListener.isOpen())
        rawLogger->log(LogLevel::Warn, __FILE__, __LINE__, "LAN discovery listener: no sockets opened");

    // Step 19: Debug console + command registry.
    DebugCommandRegistry dbgRegistry;
    DebugConsole dbgConsole(*rawLogger, dbgRegistry);
    clientHandler.console = &dbgConsole;
    localServer.registerDebugCommands(
        dbgRegistry, makeNetworkAdminSender(*clientNet, std::string(localServer.sessionToken())), renderBridge,
        &entityRegistry, &clientHandler.assignedEntityIdx, &clientHandler.assignedEntityGen, &dbgConsole.showPosRef());

    // Step 20: Per-frame state.
    CameraInput camInput;
    if (outcome == FirstRunOutcome::LaunchSandboxInspector)
        camInput.setInitialPivot({0.0, 2000.0, 0.0});

    PerformanceOverlay perfOverlay;
    perfOverlay.setMode(userConfig.debug().overlayMode);

    std::array<ParticleEmitterState, 9> precipBuf{};
    static uint32_t inputSeq = 0;

    bool running = true;
    while (running && !p.window->shouldClose()) {
        // Scroll wheel zoom — drain before pollEvents.
        if (!dbgConsole.isOpen()) {
            SDL_PumpEvents();
            SDL_Event ev;
            while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_WHEEL, SDL_EVENT_MOUSE_WHEEL) > 0) {
                const float s = ev.wheel.y;
                if (cameraController.mode() == fl::CameraMode::Free)
                    camInput.setThrottle(std::clamp(camInput.throttle(), 0.f, 1.f)); // (zoom handled in CameraInput)
                // Forward scroll to CameraInput by adjusting the radius directly.
                // CameraInput owns radius; we mutate it via a trick: negative delta = zoom in.
                (void)s; // CameraInput::update reads SDL scroll internally via SDL_PeepEvents
            }
        }

        p.window->pollEvents();
        p.renderer->beginFrame();

        // Player entity lookup from the latest snapshot.
        const fl::EntityRenderEntry* playerEntry = nullptr;
        if (renderBridge.hasSnapshot()) {
            for (const auto& e : renderBridge.current().entries)
                if (e.entityIdx == clientHandler.assignedEntityIdx && e.entityGen == clientHandler.assignedEntityGen) {
                    playerEntry = &e;
                    break;
                }
        }

        // Camera mode switches (F1/F2/F4) and backtick console toggle.
        {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            static bool f1Prev{}, f2Prev{}, f4Prev{}, gravePrev{};
            bool graveNow = keys[SDL_SCANCODE_GRAVE] != 0;
            if (graveNow && !gravePrev) {
                if (dbgConsole.isOpen())
                    dbgConsole.close(*p.input);
                else
                    dbgConsole.open(*p.input);
            }
            gravePrev = graveNow;

            if (!dbgConsole.isOpen()) {
                if (keys[SDL_SCANCODE_F1] && !f1Prev) {
                    cameraController.setMode(fl::CameraMode::Cockpit);
                    camInput.onModeSwitch(fl::CameraMode::Cockpit, playerEntry);
                }
                if (keys[SDL_SCANCODE_F2] && !f2Prev) {
                    cameraController.setMode(fl::CameraMode::Chase);
                    camInput.onModeSwitch(fl::CameraMode::Chase, playerEntry);
                }
                if (keys[SDL_SCANCODE_F4] && !f4Prev) {
                    cameraController.setMode(fl::CameraMode::Free);
                    camInput.onModeSwitch(fl::CameraMode::Free, playerEntry);
                }
            }
            f1Prev = keys[SDL_SCANCODE_F1];
            f2Prev = keys[SDL_SCANCODE_F2];
            f4Prev = keys[SDL_SCANCODE_F4];
        }

        // Per-mode camera update.
        camInput.update(cameraController, playerEntry, dbgConsole, terrainStreamer);

        // Debug console tick and inspector.
        bool consoleWasOpen = dbgConsole.isOpen();
        if (consoleWasOpen) {
            if (dbgConsole.tick(*p.input))
                dbgConsole.close(*p.input);
        }
        if (inspector && !inspector->update() && !consoleWasOpen)
            running = false;

        // Network service.
        clientNet->service(0);
        discoveryListener.poll();

        // Flight input → send MsgClientInput to local server.
        {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            fl::MsgClientInput inp;
            inp.seqNum = inputSeq++;
            inp.tickIndex = renderBridge.hasSnapshot() ? renderBridge.current().tickIndex : 0;
            constexpr float kThrottleStep = 1.0f / 60.0f;
            if (!dbgConsole.isOpen()) {
                if (keys[SDL_SCANCODE_PAGEUP])
                    camInput.adjustThrottle(kThrottleStep);
                if (keys[SDL_SCANCODE_PAGEDOWN])
                    camInput.adjustThrottle(-kThrottleStep);
                inp.throttle = keys[SDL_SCANCODE_LSHIFT] ? 1.f : camInput.throttle();
                inp.elevator = (keys[SDL_SCANCODE_UP] ? -1.f : 0.f) + (keys[SDL_SCANCODE_DOWN] ? 1.f : 0.f);
                inp.aileron = (keys[SDL_SCANCODE_RIGHT] ? 1.f : 0.f) + (keys[SDL_SCANCODE_LEFT] ? -1.f : 0.f);
                inp.rudder = (keys[SDL_SCANCODE_X] ? 1.f : 0.f) + (keys[SDL_SCANCODE_Z] ? -1.f : 0.f);
                inp.buttons = keys[SDL_SCANCODE_SPACE] ? 1u : 0u;

                // Gamepad axis blend — wins when |axis| > deadzone.
                const auto cs = userConfig.controls();
                if (p.input->getGamepadCount() > 0) {
                    const float dz = cs.gamepadDeadzone;
                    auto applyAxis = [dz](float raw) -> float {
                        float mag = std::abs(raw);
                        if (mag <= dz)
                            return 0.0f;
                        return std::copysign((mag - dz) / (1.0f - dz), raw);
                    };
                    // Throttle: TriggerLeft [0,1] → absolute set when above deadzone.
                    float trig = p.input->getGamepadAxis(0, GamepadAxis::TriggerLeft);
                    if (trig > dz) {
                        float t = (trig - dz) / (1.0f - dz);
                        camInput.setThrottle(cs.invertThrottle ? 1.0f - t : t);
                        inp.throttle = camInput.throttle();
                    }
                    float elev = applyAxis(p.input->getGamepadAxis(0, GamepadAxis::RightY));
                    if (elev != 0.0f)
                        inp.elevator = cs.invertPitch ? -elev : elev;
                    float ail = applyAxis(p.input->getGamepadAxis(0, GamepadAxis::RightX));
                    if (ail != 0.0f)
                        inp.aileron = cs.invertRoll ? -ail : ail;
                    float rud = applyAxis(p.input->getGamepadAxis(0, GamepadAxis::LeftX));
                    if (rud != 0.0f)
                        inp.rudder = cs.invertRudder ? -rud : rud;
                }

                // HOTAS / raw joystick blend — throttle always sets absolute position;
                // stick/pedal axes win when |axis| > hotasDeadzone.
                if (p.joystick && p.joystick->getJoystickCount() > 0) {
                    const int axCount = p.joystick->getAxisCount(0);
                    const float hdz = cs.hotasDeadzone;
                    auto applyHotas = [hdz](float raw) -> float {
                        float mag = std::abs(raw);
                        if (mag <= hdz)
                            return 0.0f;
                        return std::copysign((mag - hdz) / (1.0f - hdz), raw);
                    };
                    // Throttle: full-range [-1, 1] → [0, 1]; absolute position device.
                    if (cs.hotasThrottleAxis >= 0 && cs.hotasThrottleAxis < axCount) {
                        float raw = p.joystick->getAxisValue(0, cs.hotasThrottleAxis);
                        if (cs.hotasInvertThrottle)
                            raw = -raw;
                        camInput.setThrottle(std::clamp((raw + 1.0f) * 0.5f, 0.0f, 1.0f));
                        inp.throttle = camInput.throttle();
                    }
                    if (cs.hotasElevatorAxis >= 0 && cs.hotasElevatorAxis < axCount) {
                        float elev = applyHotas(p.joystick->getAxisValue(0, cs.hotasElevatorAxis));
                        if (elev != 0.0f)
                            inp.elevator = cs.hotasInvertPitch ? -elev : elev;
                    }
                    if (cs.hotasAileronAxis >= 0 && cs.hotasAileronAxis < axCount) {
                        float ail = applyHotas(p.joystick->getAxisValue(0, cs.hotasAileronAxis));
                        if (ail != 0.0f)
                            inp.aileron = cs.hotasInvertRoll ? -ail : ail;
                    }
                    if (cs.hotasRudderAxis >= 0 && cs.hotasRudderAxis < axCount) {
                        float rud = applyHotas(p.joystick->getAxisValue(0, cs.hotasRudderAxis));
                        if (rud != 0.0f)
                            inp.rudder = cs.hotasInvertRudder ? -rud : rud;
                    }
                }
            } else {
                inp.throttle = camInput.throttle();
            }
            clientNet->send(0, &inp, sizeof(inp), /*reliable=*/true);
        }

        // Render.
        float alpha = clientHandler.tickAlpha.get();
        float aspect =
            static_cast<float>(p.window->width()) / static_cast<float>(p.window->height() > 0 ? p.window->height() : 1);
        CameraView cam = cameraController.view(aspect);

        p.asyncFilesystem->service();
        terrainStreamer.update(cam.worldOrigin);

        // Audio listener.
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

        const AudioSettings& aud = userConfig.audio();
        subtitleQueue.update(1.0f / 60.0f);
        musicManager.update(1.0f / 60.0f, aud.masterVolume, aud.musicVolume);

        std::span<const ParticleEmitterState> sandboxEmitters{};
        if (env.cloudCoverage >= 0.75f) {
            const char* presetName = env.cloudCoverage >= 0.90f ? "storm_rain" : "rain";
            if (auto preset = particleSystem.getPreset(presetName)) {
                const glm::vec3 camF = glm::vec3(cam.worldOrigin);
                int idx = 0;
                for (int gx = -1; gx <= 1; ++gx) {
                    for (int gz = -1; gz <= 1; ++gz, ++idx) {
                        ParticleEmitterState& e = precipBuf[idx];
                        e = ParticleEmitterState{};
                        e.position = camF + glm::vec3(gx * 40.0f, 60.0f, gz * 40.0f);
                        e.effectName = presetName;
                        e.intensity = 1.0f;
                        e.spawnRate = preset->spawnRate;
                        e.particleLifetime = preset->particleLifetime;
                        e.initialSpeed = preset->initialSpeed;
                        e.colorStart = preset->colorStart;
                        e.colorEnd = preset->colorEnd;
                        e.sizeStart = preset->sizeStart;
                        e.sizeEnd = preset->sizeEnd;
                        e.additive = preset->additive;
                        e.emitDirection = preset->emitDirection;
                    }
                }
                sandboxEmitters = {precipBuf.data(), 9};
            }
        }

        sceneRenderer.renderFrame(alpha, cam, env, sandboxEmitters);

        // HUD: GameHud (flight data + notices) and DebugConsole are independent layers.
        const float terrainElev =
            playerEntry ? static_cast<float>(terrainStreamer.heightAt(playerEntry->position.x, playerEntry->position.z))
                        : 0.0f;
        gameHud.update(cameraController.mode(), playerEntry, env.timeOfDay, terrainElev);
        {
            glm::dvec3 playerPos{};
            const glm::dvec3* playerPosPtr = nullptr;
            if (playerEntry) {
                playerPos = playerEntry->position;
                playerPosPtr = &playerPos;
            }
            dbgConsole.buildHud(playerPosPtr);
        }

        // F3 performance overlay.
        {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            static bool f3Prev = false;
            if (!dbgConsole.isOpen() && keys[SDL_SCANCODE_F3] && !f3Prev) {
                perfOverlay.cycleMode();
                DebugSettings ds = userConfig.debug();
                ds.overlayMode = perfOverlay.mode();
                userConfig.setDebug(ds);
                userConfig.save();
            }
            f3Prev = keys[SDL_SCANCODE_F3];

            uint32_t entityCount =
                renderBridge.hasSnapshot() ? static_cast<uint32_t>(renderBridge.current().entries.size()) : 0u;
            perfOverlay.update(p.renderer->getFrameStats(), entityCount, 1000.0f / 60.0f);
            p.renderer->setOverlayLines(perfOverlay.lines());
        }

        // Merge HUD layers and submit.
        {
            auto hudElems = gameHud.buildElements();
            auto dbgElems = dbgConsole.elements();
            std::vector<HudElement> allHud(hudElems.begin(), hudElems.end());
            allHud.insert(allHud.end(), dbgElems.begin(), dbgElems.end());
            p.renderer->submitHudElements(allHud);
        }

        p.renderer->endFrame();
        p.input->flush();
        p.joystick->flush();
    }

    // Step 21: Clean shutdown.
    localServer.stop();
    clientNet->disconnect();
    clientNet->shutdown();
    inspector.reset();
    musicManager.shutdown();
    p.cursor.reset();
    p.audio->shutdown();
    p.renderer->shutdown();
    p.window->shutdown();
    crashReporter.shutdown();
    return 0;
}
