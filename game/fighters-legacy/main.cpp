// SPDX-License-Identifier: GPL-3.0-or-later
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
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "firstrun/FirstRun.h"
#include "loop/GameLoop.h"
#include "loop/GameState.h"
#include "openal/OALAudio.h"
#include "render/CameraController.h"
#include "render/ParticleSystem.h"
#include "render/SceneRenderer.h"
#include "render/SimRenderBridge.h"
#include "sandbox/SandboxInspector.h"
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

namespace fs = std::filesystem;

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

    // Step 17b: Render bridge — wired before the sim thread starts so EntityManager::onTick
    // can publish snapshots from the very first tick.
    fl::SimRenderBridge renderBridge;
    entityManager.setRenderBridge(&renderBridge);

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

        // Enable the builtin floor plane (4 km × 4 km flat quad at Y=0).
        sceneRenderer.setBuiltinFloor(true);

        // Orbit the camera south of the formation (yaw=0 = south of pivot), looking north.
        cameraController.setFreeOrbit({0.0f, 500.0f, 0.0f}, 0.0f, -10.0f, 200.0f);

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

    // Step 17d: Game loop — sim thread starts here.
    GameLoop gameLoop(entityManager, *rawLogger);
    gameLoop.start();

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
    glm::vec3 sbPivot{0.0f, 500.0f, 0.0f}; // mutable — WASD/QE pan it
    float sbYaw = 0.0f;
    float sbPitch = -10.0f;
    float sbRadius = 200.0f;
    float sbLastMouseX = 0.0f;
    float sbLastMouseY = 0.0f;
    bool sbFirstFrame = true;

    bool running = true;
    while (running && !p.window->shouldClose()) {
        // Sandbox: pull wheel events out of the SDL queue before pollEvents drains it.
        if (inspector) {
            SDL_PumpEvents();
            SDL_Event ev;
            while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_WHEEL, SDL_EVENT_MOUSE_WHEEL) > 0)
                sbRadius = std::clamp(sbRadius - ev.wheel.y * 10.0f, 20.0f, 5000.0f);
        }

        p.window->pollEvents();
        p.renderer->beginFrame();

        // Sandbox free-look: mouse orbit + keyboard zoom/reset.
        if (inspector) {
            float mx = 0, my = 0;
            SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
            if (!sbFirstFrame && (mb & SDL_BUTTON_LMASK)) {
                sbYaw -= (mx - sbLastMouseX) * 0.35f;
                sbPitch += (my - sbLastMouseY) * 0.25f;
                sbPitch = std::clamp(sbPitch, -89.0f, 89.0f);
            }
            sbLastMouseX = mx;
            sbLastMouseY = my;
            sbFirstFrame = false;

            const bool* keys = SDL_GetKeyboardState(nullptr);

            // Zoom.
            if (keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS])
                sbRadius = std::max(20.0f, sbRadius - 5.0f);
            if (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS])
                sbRadius = std::min(5000.0f, sbRadius + 5.0f);

            // WASD — pan pivot in camera-relative horizontal plane.
            // Q/E — altitude (world Y).
            {
                const float speed = std::max(1.0f, sbRadius * 0.01f);
                const float yawRad = glm::radians(sbYaw);
                const glm::vec3 fwd{-std::sin(yawRad), 0.0f, -std::cos(yawRad)};
                const glm::vec3 rgt{std::cos(yawRad), 0.0f, -std::sin(yawRad)};
                if (keys[SDL_SCANCODE_W])
                    sbPivot += fwd * speed;
                if (keys[SDL_SCANCODE_S])
                    sbPivot -= fwd * speed;
                if (keys[SDL_SCANCODE_D])
                    sbPivot += rgt * speed;
                if (keys[SDL_SCANCODE_A])
                    sbPivot -= rgt * speed;
                if (keys[SDL_SCANCODE_E])
                    sbPivot.y += speed;
                if (keys[SDL_SCANCODE_Q])
                    sbPivot.y -= speed;
            }

            // Reset to initial formation view.
            if (keys[SDL_SCANCODE_R]) {
                sbPivot = {0.0f, 500.0f, 0.0f};
                sbYaw = 0.0f;
                sbPitch = -10.0f;
                sbRadius = 200.0f;
            }

            cameraController.setFreeOrbit(sbPivot, sbYaw, sbPitch, sbRadius);
        }

        if (inspector && !inspector->update())
            running = false;

        float alpha = gameLoop.shellTick();
        float aspect =
            static_cast<float>(p.window->width()) / static_cast<float>(p.window->height() > 0 ? p.window->height() : 1);
        CameraView cam = cameraController.view(aspect);

        // Update audio listener from camera pose each frame.
        // Column-major glm::mat4: view[col][row]. Column 2 = -forward (RH); column 1 = up.
        {
            glm::vec3 fwd = -glm::vec3(cam.view[2][0], cam.view[2][1], cam.view[2][2]);
            glm::vec3 up = glm::vec3(cam.view[1][0], cam.view[1][1], cam.view[1][2]);
            const float pos[3] = {cam.worldOrigin.x, cam.worldOrigin.y, cam.worldOrigin.z};
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
        p.renderer->endFrame();
        p.input->flush();
        p.joystick->flush();
    }

    // Step 19: Clean shutdown.
    gameLoop.stop(); // join sim thread before any HAL teardown
    inspector.reset();
    musicManager.shutdown(); // must come before audio shutdown
    p.cursor.reset();        // destroy cursor while SDL video is still alive (before SDL_Quit)
    p.audio->shutdown();
    p.renderer->shutdown();
    p.window->shutdown();
    crashReporter.shutdown();
    return 0;
}
