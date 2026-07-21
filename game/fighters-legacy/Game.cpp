// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#include "Game.h"

#include "CameraInput.h"
#include "ClientEffectRouter.h"
#include "ClientNetEventHandler.h"
#include "CommsMenu.h"
#include "DebriefScreen.h"
#include "EngineAudioManager.h"
#include "FileLogger.h"
#include "FlightInputCollector.h"
#include "FlightScreen.h"
#include "HapticController.h"
#include "HeadlessHal.h"
#include "IWindowEventHandler.h"
#include "LocalServer.h"
#include "MainMenuScreen.h"
#include "MissionBriefScreen.h"
#include "MissionSelectScreen.h"
#include "NetworkFactory.h"
#include "Platform.h"
#include "PrecipitationController.h"
#include "RecordScheduler.h"
#include "ScreenManager.h"
#include "ServerNotice.h"
#include "SessionStatus.h"
#include "SubtitleOverlay.h"
#include "Version.h"
#include "VideoEncoderPipe.h"
#include "WingmanMenu.h"
#include "audio/MusicBuiltinTracks.h"
#include "audio/MusicManager.h"
#include "audio/PlaylistLoader.h"
#include "audio/SfxManager.h"
#include "audio/SubtitleQueue.h"
#include "audio/VoiceCalloutManager.h"
#include "audio/WarningToneManager.h"
#include "config/ConfigFile.h"
#include "config/UserConfig.h"
#include "console/CommandRegistry.h"
#include "console/GameConsole.h"
#include "content/AssetManager.h"
#include "content/BundledBaseTerrain.h"
#include "content/ModLoader.h"
#include "crash/CrashInfo.h"
#include "crash/CrashReporter.h"
#include "entity/BuiltinShapeMap.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
#include "firstrun/FirstRun.h"
#include "flight/Atmosphere.h"
#include "flight/Geodetic.h"
#include "http/CurlHttpClientFactory.h" // createHttpClient (#490)
#include "input/AxisConfig.h"
#include "input/InputBindings.h"
#include "mission/MissionParser.h"
#include "mission/ShotDirector.h"
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
#include "sdl3/SDL3Cursor.h"
#include "sdl3/SDL3Display.h"
#include "sdl3/SDL3Input.h"
#include "sdl3/SDL3Joystick.h"
#include "sdl3/SDL3Window.h"
#include "stdfs/StdAsyncFilesystem.h"
#include "stdfs/StdFilesystem.h"
#include "vulkan/VkRendererFactory.h"
#include "weather/WeatherController.h" // applyGeographicSun — per-observer sun (#481)

#include "ClientFlightModelResolver.h"
#include "ClientPrediction.h"
#include "ConnectArgs.h"
#include "ManualOverlay.h"
#include "console/ConsoleCommands.h"
#include "content/ContentBootstrap.h"
#include "content/ContentIndex.h"
#include "entity/EntityDefParser.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightModelParser.h"
#include "manual/AircraftManual.h"
#include "render/RunwaySurfaceMap.h"
#include "sensor/SensorDefParser.h"
#include "weapon/WeaponRegistry.h"
#include "world/AirportBootstrap.h"
#include "world/AirportRegistry.h"
#include "world/BuiltinAirport.h"

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

namespace fl {

// ---------------------------------------------------------------------------
// File-scope helpers
// ---------------------------------------------------------------------------

// Resolve where the client looks for content packs (the mods/ directory) and all other assets.
// The dedicated server roots content at the current working directory; the client historically used
// only the executable directory (SDL_GetBasePath), so a `mods/` staged in the CWD -- the normal dev
// layout -- was invisible to the client while it loaded fine on the server (#831). Resolution order:
//   1. --assets <dir>      explicit CLI override
//   2. FL_ASSETS_ROOT env  explicit environment override
//   3. <exe-dir>           if <exe-dir>/mods exists (release: packs staged beside the binary)
//   4. <cwd>               if <cwd>/mods exists (dev: run from a directory containing mods/)
//   5. <exe-dir>           default (no packs; the zero-content sandbox)
static fs::path resolveAssetsRoot(const fs::path& exeDir, int argc, char** argv, ILogger& log) {
    auto choose = [&](fs::path root, const char* why) {
        log.log(LogLevel::Info, __FILE__, __LINE__,
                (std::string("assets root: ") + root.string() + " (" + why + ")").c_str());
        return root;
    };

    for (int i = 1; i < argc - 1; ++i)
        if (std::strcmp(argv[i], "--assets") == 0)
            return choose(fs::path(argv[i + 1]), "--assets");

    if (const char* ev = SDL_getenv("FL_ASSETS_ROOT"); ev && *ev)
        return choose(fs::path(ev), "FL_ASSETS_ROOT");

    std::error_code ec;
    if (fs::exists(exeDir / "mods", ec))
        return choose(exeDir, "beside executable");

    const fs::path cwd = fs::current_path(ec);
    if (!ec && fs::exists(cwd / "mods", ec))
        return choose(cwd, "current directory");

    return choose(exeDir, "default; no mods/ directory found");
}

static std::function<void(std::string_view)> makeNetworkAdminSender(INetwork& net, std::string token) {
    return [&net, tok = std::move(token), nextReqId = uint16_t{1}](std::string_view cmd) mutable {
        fl::MsgAdminCommand msg{};
        msg.msgId = static_cast<uint8_t>(fl::MsgId::AdminCommand);
        msg.reqId = nextReqId++;
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
    // Ordinals must stay in sync with the enum definitions in both headers.
    s.aaMode = static_cast<RendererAAMode>(g.aaMode);
    s.shadowQuality = static_cast<RendererShadowQuality>(g.shadowQuality);
    s.particleDensity = static_cast<RendererParticleDensity>(g.particleDensity);
    s.aoMode = static_cast<RendererAOMode>(g.ambientOcclusion);
    s.skyQuality = static_cast<RendererSkyQuality>(g.skyQuality);
    s.autoExposure = true; // baseline HDR feature, always on
    s.bloom = (g.qualityPreset >= QualityLevel::Medium);
    switch (g.drawDistance) {
    case DrawDistance::Low:
        s.drawDistanceKm = 20.0f;
        break;
    case DrawDistance::Medium:
        s.drawDistanceKm = 50.0f;
        break;
    case DrawDistance::Ultra:
        s.drawDistanceKm = 200.0f;
        break;
    default:
        s.drawDistanceKm = 100.0f;
        break; // High
    }
    return s;
}

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
                              const fl::SimRenderBridge& bridge, UserConfig& userConfig, bool inFlight,
                              fl::CameraMode camMode, const CameraView& cam, const fl::EntityRenderEntry* playerEntry,
                              fl::TerrainStreamer* terrain) {
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

    // Append live camera + entity readouts (so the underground/aim issues are visible in real time).
    if (overlay.mode() != OverlayMode::Off) {
        const char* modeStr = camMode == fl::CameraMode::Cockpit ? "COCKPIT"
                              : camMode == fl::CameraMode::Chase ? "CHASE"
                                                                 : "FREE";
        const double planetR = terrain ? terrain->planetRadiusM() : fl::kEarthRadiusM;
        const double terrCam = terrain ? terrain->heightAt(cam.worldOrigin) : 0.0;
        const double terrEnt = (terrain && playerEntry) ? terrain->heightAt(playerEntry->position) : 0.0;
        overlay.setSceneInfo(modeStr, cam, playerEntry ? &playerEntry->position : nullptr, terrCam, terrEnt, planetR);
    }

    renderer.setOverlayLines(overlay.lines());
}

struct ResizeHandler : IWindowEventHandler {
    IRenderer* r = nullptr;
    void onResize(int w, int h) override {
        r->onResize(w, h);
    }
    void onClose() override {}
};

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
    fl::InputBindings inputBindings;     // loaded from config/bindings.toml; stored for Phase 4
    fl::AxisConfigTable axisConfigTable; // loaded from config/bindings.toml [axis_config]
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
    // Runway airports (#486): loaded from the same bundled data as the server so the runway
    // terrain-flatten agrees on both ends. Held via unique_ptr because AirportRegistry is
    // non-copyable/non-movable; rebuilt on each session's MsgConnectAck.
    std::unique_ptr<fl::AirportRegistry> airportRegistry;
    SubtitleQueue subtitleQueue;
    MusicManager musicManager;
    fl::SfxManager sfxManager;           // positional weapon SFX (#631)
    fl::WarningToneManager warningTones; // stall / overspeed cockpit tones (#957)
    fl::EngineAudioManager engineAudio;  // continuous engine + aero doppler layers (#959)
    AudioSettings audioSettings{};       // a stable copy the effect router points at; refreshed on apply

    // Multiplayer connection target (empty = single-player, spawn LocalServer).
    // Populated from --connect CLI arg in initPlatform().
    std::string connectHost;
    uint16_t connectPort{4778};
    std::string operatorPassword; // merged: CLI arg > FL_OPERATOR_PASSWORD > [client].operator_password
    std::string joinPassword;     // per-session join password for a private server (#998); not persisted
    std::string
        requestedEntityType;     // --aircraft: aircraft to request in MsgConnectRequest; empty = server default (#834)
    bool requestObserver{false}; // --observer: join as a spectator (no aircraft) (#857)
    // Auto-start (menu bypass): skip the main menu and enter a session immediately, exactly
    // as if the matching menu item had been confirmed. `--mission <id>` implies it (single-player
    // with that mission); `--auto` alone enters Free Flight, or Join Server when --connect is set.
    bool autoStart{false};
    std::string autoStartMission; // mission id for --mission; empty = Free Flight / Join Server
    // Automated frame capture (#909 groundwork): --screenshot <path> writes one PNG this many frames
    // after the Flight session starts, then quits — the reliable in-engine visual-verification path.
    std::string screenshotPath;
    int screenshotFrames{600}; // ~10 s at 60 fps: enough for terrain + airports to stream in

    // Headless client (#913): no window, no display, swapchain-free renderer (pair with a software
    // Vulkan ICD like lavapipe for no-GPU rendering). The camera is driven by the recorder, not input.
    bool headless{false};
    int headlessW{1280};
    int headlessH{720};

    // Cinematic recorder (#916): drives the camera from the mission's `cameras:` shots via ShotDirector
    // and pipes rendered frames to ffmpeg (mp4) or a PNG sequence. Active when --record/--record-png-dir
    // is set. The capture-boundary scheduler emits one frame per boundary and fails loud on drops.
    struct Recorder {
        bool active{false};
        std::string outPath;       // --record <out.mp4>
        std::string pngDir;        // --record-png-dir <dir> (ffmpeg-free fallback)
        std::string shotTrackPath; // --shot-track <yaml>; empty = fall back to the --mission file
        int fps{30};
        bool exitOnMissionEnd{false};
        int maxSec{0};        // wall-clock recording cap in seconds; 0 = no cap
        uint64_t maxDup{300}; // duplicated-frame cap; exceeding it fails the run (non-zero exit)

        std::unique_ptr<fl::ShotDirector> director;
        std::optional<fl::RecordScheduler> scheduler;
        fl::VideoEncoderPipe encoder;

        std::vector<uint8_t> lastFrame;       // most recent captured RGBA frame (from the sink)
        uint32_t lastW{0}, lastH{0};          // its dimensions
        uint64_t lastFrameIndex{UINT64_MAX};  // renderer frameIndex of lastFrame (dup detection)
        uint64_t lastPushedIndex{UINT64_MAX}; // frameIndex last pushed to the encoder
        float curFovDeg{60.f};                // FOV of the active shot, fed into view()
        uint64_t startWallNs{0};              // wall clock at first recorded frame (maxSec)
        bool started{false};                  // has the first frame been pushed
        bool encoderFailed{false};            // a push/open error occurred
    };
    Recorder recorder;
    int exitCode{0};

    // HUD / overlays
    EnvironmentState env;
    fl::FlightHud flightHud;
    fl::IHud* activeHud{nullptr};
    fl::WindshieldRain windshieldRain;
    ServerNotice serverNotice;
    WingmanMenu wingmanMenu;           // the radio menu for ordering your flight (#610)
    CommsMenu commsMenu;               // the ATC comms menu (#704)
    VoiceCalloutManager voiceCallouts; // resolved-text radio callouts -> subtitle + optional voice (#704)
    SubtitleOverlay subtitleOverlay;   // renders the subtitle queue as a HUD overlay layer (#704)
    ManualOverlay manual;              // the in-flight aircraft manual, generated from the flight model (#821)
    WeaponRegistry weapons;            // the client's copy of the pack's stores, for the manual's loadout section
    ContentIndex contentIndex;         // id -> asset name, so the client can resolve def cross-references (#810)

    // Debug console
    CommandRegistry cmdRegistry;
    std::optional<GameConsole> gameConsole;

    // Per-frame state
    CameraInput camInput;
    PerformanceOverlay perfOverlay;
    bool showPing{false}; // toggled by the show_ping console command
    FlightInputCollector flightInput;
    PrecipitationController precipController;
    ClientEffectRouter effectRouter;                 // cosmetic weapon effects (#625)
    std::vector<ParticleEmitterState> frameEmitters; // per-frame scratch: precip + effects

    // Screen state machine
    std::unique_ptr<ScreenManager> screenMgr;

    // Client-side prediction — persists across sessions; reset() on stopGame().
    ClientPrediction prediction;
};

// Per-session objects — created in startGame(), torn down in stopGame(). Hold pointers/refs into
// GameServices, so they are destroyed first (declared after services in GameImpl); stopGame() also
// empties them before ~GameImpl, so the program-exit destruction order is moot.
struct SessionContext {
    std::optional<LocalServer> localServer;
    std::unique_ptr<INetwork> clientNet;
    std::unique_ptr<ClientNetEventHandler> clientHandler;
    std::optional<HapticController> hapticController;
    std::optional<DiscoveryListener> discoveryListener;
    std::optional<SandboxInspector> inspector;

    std::thread serverThread;
    std::atomic<bool> serverReady{false};
    // Typed session failure, first-writer-wins (server thread + ClientNetEventHandler write;
    // LoadingScreen reads). Replaces the prior two atomic<const char*> + static-string signals.
    std::atomic<SessionFailure> sessionFailure{SessionFailure::None};
    // Set once per session at the first tick after MsgConnectAck (assignedEntityGen != 0):
    // applies the server's planet radius to the terrain streamer + camera (terrain streaming
    // is gated on it — tiles bake the radius at generation time) and wires ClientPrediction
    // with the post-ack player idx/gen/radius (#755).
    bool connectAckApplied{false};
    // The entity gen + role the connect-ack setup last applied. A mid-session role switch (server
    // set_role, #859/#648) re-sends MsgConnectAck with a different entity/role; when these change the
    // setup re-runs so an observer→pilot switch wires prediction + the cockpit, and pilot→observer
    // tears prediction down and drops back to the ghost camera.
    uint32_t appliedEntityGen{0};
    fl::PeerRole appliedRole{fl::PeerRole::Pilot};
    bool manualBuilt{false}; // the aircraft manual is generated once per session (#821)
    // Wall-clock start of this session; the delta at debrief accrues into PilotProfile::flightTimeS (#634).
    std::chrono::steady_clock::time_point sessionStart{};
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
    d.services.sfxManager.shutdown();
    d.services.warningTones.shutdown();
    d.services.engineAudio.shutdown();
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
    if (!initRecorder()) // #916: build the ShotDirector + open the encoder; failure aborts init
        return false;
    initGameSystems();
    initGameConsole();
    initScreenManager();
    return true;
}

int Game::exitCode() const {
    return m_impl->services.exitCode;
}

bool Game::initRecorder() {
    auto& d = *m_impl;
    auto& rec = d.services.recorder;
    if (!rec.active)
        return true;

    // Load the camera shots: the --shot-track sidecar if given, else the --mission file (a readable
    // path). Both parse through the single schema owner (parseMission), so a cameras-only sidecar and a
    // full mission with a cameras: block are handled identically.
    const std::string shotFile = !rec.shotTrackPath.empty() ? rec.shotTrackPath : d.services.autoStartMission;
    std::vector<fl::MissionShot> shots;
    if (!shotFile.empty()) {
        std::ifstream f(shotFile, std::ios::binary);
        if (f) {
            std::string yaml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            fl::MissionParseResult parsed = fl::parseMission(yaml);
            shots = std::move(parsed.mission.shots);
        } else {
            d.services.rawLogger->log(LogLevel::Warn, __FILE__, __LINE__,
                                      ("recorder: cannot open shot track \"" + shotFile + "\"").c_str());
        }
    }
    if (shots.empty())
        d.services.rawLogger->log(LogLevel::Warn, __FILE__, __LINE__,
                                  "recorder: no camera shots found — the camera will hold a default pose");
    rec.director = std::make_unique<fl::ShotDirector>(std::move(shots));
    rec.scheduler.emplace(rec.fps);

    // Open the encoder: a PNG sequence if --record-png-dir, else ffmpeg mp4. FL_FFMPEG overrides the
    // executable. A failure to open is fatal — a recording run must not silently produce nothing.
    const uint32_t w = static_cast<uint32_t>(d.services.headlessW);
    const uint32_t h = static_cast<uint32_t>(d.services.headlessH);
    bool opened = false;
    if (!rec.pngDir.empty()) {
        opened = rec.encoder.openPngDir(rec.pngDir, w, h);
    } else {
        const char* ff = SDL_getenv("FL_FFMPEG");
        opened = rec.encoder.openFfmpeg(rec.outPath, w, h, rec.fps, ff ? ff : "");
    }
    if (!opened) {
        d.services.rawLogger->log(LogLevel::Error, __FILE__, __LINE__,
                                  rec.encoder.lastError() ? rec.encoder.lastError() : "recorder: encoder open failed");
        return false;
    }
    rec.lastFrame.assign(static_cast<std::size_t>(w) * h * 4u, 0);
    rec.lastW = w;
    rec.lastH = h;

    // Install the capture sink: copy each rendered frame into lastFrame. The record loop pushes it to
    // the encoder at capture boundaries (dup detection keys on the renderer frameIndex).
    d.services.p.renderer->setCaptureSink([&rec](const fl::CaptureFrame& cf) {
        const std::size_t bytes = static_cast<std::size_t>(cf.width) * cf.height * 4u;
        if (cf.pixels && cf.width == rec.lastW && cf.height == rec.lastH && rec.lastFrame.size() == bytes) {
            std::memcpy(rec.lastFrame.data(), cf.pixels, bytes);
            rec.lastFrameIndex = cf.frameIndex;
        }
    });
    d.services.rawLogger->log(LogLevel::Info, __FILE__, __LINE__, "recorder: armed");
    return true;
}

void Game::driveRecorderCamera() {
    auto& d = *m_impl;
    auto& rec = d.services.recorder;
    if (!rec.director || !d.services.renderBridge.hasSnapshot() || !d.session.clientHandler)
        return;
    const fl::RenderSnapshot& snap = d.services.renderBridge.current();
    const double simTime = static_cast<double>(snap.tickIndex) / 60.0;
    auto* handler = d.session.clientHandler.get();
    // Resolve a mission object id -> its live world pose via the #914 roster + the render snapshot.
    auto poseOf = [&](std::string_view id, glm::dvec3& pos, glm::dquat& orient) -> bool {
        uint32_t idx = 0;
        uint16_t gen = 0;
        if (!handler->missionEntity(std::string(id), idx, gen))
            return false;
        for (const auto& e : snap.entries) {
            if (e.entityIdx == idx && e.entityGen == gen) {
                pos = e.position;
                orient = glm::dquat(e.orientation);
                return true;
            }
        }
        return false;
    };
    const fl::ShotPose sp = rec.director->evaluate(simTime, poseOf);
    d.services.cameraController.setMode(fl::CameraMode::Free); // the recorder owns the pose
    d.services.cameraController.setPose(sp.eye, sp.fwd, sp.up);
    rec.curFovDeg = sp.fovYDeg;
}

void Game::recorderEmit(bool& running) {
    auto& d = *m_impl;
    auto& rec = d.services.recorder;
    if (!rec.scheduler || !d.services.renderBridge.hasSnapshot())
        return;
    const uint64_t latestTick = d.services.renderBridge.current().tickIndex;
    const uint64_t nowNs = SDL_GetTicksNS();
    if (!rec.started) {
        rec.startWallNs = nowNs;
        rec.started = true;
    }
    // Push one video frame per capture boundary reached since the last call. The scheduler counts how
    // many of these are duplicates (the sim advanced past more than one boundary between renders).
    const int frames = rec.scheduler->boundariesDue(latestTick);
    for (int k = 0; k < frames; ++k) {
        if (!rec.encoder.pushFrame(rec.lastFrame.data(), rec.lastW, rec.lastH)) {
            rec.encoderFailed = true;
            running = false;
            return;
        }
        rec.lastPushedIndex = rec.lastFrameIndex;
    }

    // Stop conditions: the shot list ran out, the mission objective ended (--exit-on-mission-end), or the
    // wall-clock safety cap tripped.
    const double simTime = static_cast<double>(latestTick) / 60.0;
    const bool tracksDone =
        rec.director && rec.director->totalDurationSec() > 0.0 && simTime >= rec.director->totalDurationSec();
    const bool missionEnded = rec.exitOnMissionEnd && d.session.clientHandler &&
                              d.session.clientHandler->missionOutcome() != fl::MissionResultCode::Incomplete;
    const bool timedOut =
        rec.maxSec > 0 && (nowNs - rec.startWallNs) >= static_cast<uint64_t>(rec.maxSec) * 1'000'000'000ull;
    if (tracksDone || missionEnded || timedOut)
        running = false;
}

void Game::recorderFinish() {
    auto& d = *m_impl;
    auto& rec = d.services.recorder;
    if (!rec.active)
        return;
    // Detach the sink before tearing the encoder down so no late frame reaches a closed pipe.
    if (d.services.p.renderer)
        d.services.p.renderer->setCaptureSink({});
    const bool encoderOk = rec.encoder.close() && !rec.encoderFailed;
    const uint64_t dups = rec.scheduler ? rec.scheduler->dupFrames() : 0;
    const uint64_t total = rec.scheduler ? rec.scheduler->totalFrames() : 0;
    const bool dupExceeded = dups > rec.maxDup;

    char msg[224];
    std::snprintf(msg, sizeof(msg), "recorder: %llu frame(s), %llu duplicate(s) (cap %llu); encoder %s",
                  static_cast<unsigned long long>(total), static_cast<unsigned long long>(dups),
                  static_cast<unsigned long long>(rec.maxDup), encoderOk ? "ok" : "FAILED");
    d.services.rawLogger->log(encoderOk && !dupExceeded ? LogLevel::Info : LogLevel::Error, __FILE__, __LINE__, msg);

    if (!encoderOk || dupExceeded)
        d.services.exitCode = 1; // bad video is loud: non-zero exit fails the record_demo.py run
}

// Steps 1–7: logger, filesystem, user config, audio, input.
bool Game::initPlatform(int argc, char** argv) {
    auto& d = *m_impl;

    SDL_Init(0);
    char* prefRaw = SDL_GetPrefPath("mkzsystems", "fighters-legacy");
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
    const fs::path exeDir = baseRaw ? fs::path(baseRaw) : fs::path(".");
    d.services.assetsRoot = resolveAssetsRoot(exeDir, argc, argv, *d.services.rawLogger);
    d.services.p.filesystem = std::make_unique<StdFilesystem>(d.services.assetsRoot, d.services.userDataDir);

    d.services.userConfig.emplace(*d.services.p.filesystem, *d.services.rawLogger);
    d.services.userConfig->load();

    // Load config/bindings.toml — writes defaults on first run.
    // InputBindings provides [primary]/[alt]; AxisConfigTable provides [axis_config].
    // Both deserializers accept the full file content (non-overlapping TOML sections).
    // Pass fs::path directly to avoid Windows locale-encoding issues from std::string conversion.
    {
        const fs::path bindingsPath = d.services.userDataDir / "config" / "bindings.toml";
        const std::string defaults = fl::InputBindings{}.serialize() + "\n" + fl::AxisConfigTable{}.serialize();
        const std::string content = fl::ensureAndReadConfig(bindingsPath, defaults, *d.services.rawLogger);
        d.services.inputBindings.deserialize(content);
        d.services.axisConfigTable.deserialize(content);
        d.services.flightInput.setBindings(d.services.inputBindings);
        d.services.flightInput.setAxisConfig(d.services.axisConfigTable);
    }

    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--log-level") == 0)
            d.services.rawLogger->setMinLevel(parseLogLevel(argv[i + 1]));
        else if (std::strcmp(argv[i], "--connect") == 0)
            parseConnectArg(argv[i + 1], d.services.connectHost, d.services.connectPort);
        else if (std::strcmp(argv[i], "--operator-password") == 0)
            d.services.operatorPassword = argv[i + 1];
        else if (std::strcmp(argv[i], "--aircraft") == 0)
            d.services.requestedEntityType = argv[i + 1]; // request a specific type; server clamps (#834)
        else if (std::strcmp(argv[i], "--screenshot") == 0)
            d.services.screenshotPath = argv[i + 1];
        else if (std::strcmp(argv[i], "--screenshot-frames") == 0)
            d.services.screenshotFrames = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--mission") == 0) {
            // Launch straight into a single-player session with this mission — the id is
            // forwarded to the embedded fl-server exactly as Instant Action passes builtin:sandbox.
            d.services.autoStartMission = argv[i + 1];
            d.services.autoStart = true;
        }
        // ── Cinematic recorder (#916) ─────────────────────────────────────────
        else if (std::strcmp(argv[i], "--record") == 0) {
            d.services.recorder.outPath = argv[i + 1];
            d.services.recorder.active = true;
        } else if (std::strcmp(argv[i], "--record-png-dir") == 0) {
            d.services.recorder.pngDir = argv[i + 1];
            d.services.recorder.active = true;
        } else if (std::strcmp(argv[i], "--record-fps") == 0) {
            d.services.recorder.fps = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--record-res") == 0) {
            int w = 0, h = 0;
            if (std::sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                d.services.headlessW = w;
                d.services.headlessH = h;
            }
        } else if (std::strcmp(argv[i], "--shot-track") == 0) {
            d.services.recorder.shotTrackPath = argv[i + 1];
        } else if (std::strcmp(argv[i], "--record-max-sec") == 0) {
            d.services.recorder.maxSec = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--record-max-dup") == 0) {
            d.services.recorder.maxDup = static_cast<uint64_t>(std::max(0, std::atoi(argv[i + 1])));
        }
    }

    // Value-less flags (scanned separately so they work as the final argument too).
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--observer") == 0)
            d.services.requestObserver = true; // join as a spectator, no aircraft (#857)
        else if (std::strcmp(argv[i], "--auto") == 0)
            d.services.autoStart = true; // menu bypass: Free Flight, or Join Server with --connect
        else if (std::strcmp(argv[i], "--headless") == 0)
            d.services.headless = true; // no window/display; swapchain-free renderer (#913)
        else if (std::strcmp(argv[i], "--exit-on-mission-end") == 0)
            d.services.recorder.exitOnMissionEnd = true; // stop recording at the objective outcome (#916)
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

    if (d.services.headless) {
        // Headless client (#913): no OS window, no input sink, no display/cursor backends. The renderer
        // presents to owned images (initHeadless) instead of a swapchain; paired with a software Vulkan
        // ICD (lavapipe) it renders with no display and no GPU. The camera is driven by the recorder.
        const int hw = d.services.headlessW, hh = d.services.headlessH;
        d.services.p.window = std::make_unique<HeadlessWindow>(hw, hh);
        d.services.p.window->init("Fighters Legacy", hw, hh);
        d.services.p.display = std::make_unique<HeadlessDisplay>();
        // p.cursor stays null (never dereferenced headless — Settings is unreachable).

        d.services.p.renderer = createVulkanRenderer();
        if (!d.services.p.renderer->initHeadless(static_cast<uint32_t>(hw), static_cast<uint32_t>(hh))) {
            d.services.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "headless renderer init failed");
            return false;
        }
        d.services.crashReporter.setGpuInfo(d.services.p.renderer->gpuInfo());
        d.services.rendererSettings = buildRendererSettings(d.services.userConfig->graphics());
        d.services.p.renderer->applySettings(d.services.rendererSettings);

        auto asyncFsH = std::make_unique<StdAsyncFilesystem>(d.services.assetsRoot, d.services.userDataDir);
        if (!asyncFsH->init()) {
            d.services.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, asyncFsH->getLastError());
            return false;
        }
        d.services.p.asyncFilesystem = std::move(asyncFsH);
        return true;
    }

    d.services.p.window = std::make_unique<SDL3Window>();

    CrashReporter::checkPreviousCrash(d.services.userDataDir.string(), d.services.p.window.get(), d.services.rawLogger,
                                      "https://github.com/fighters-legacy/fighters-legacy/issues/new");

    d.services.crashInfo.engineVersion = FL_VERSION_STRING;
    d.services.crashInfo.populateOS();
    d.services.crashReporter.init({d.services.userDataDir.string(),
                                   "https://github.com/fighters-legacy/fighters-legacy/issues/new",
                                   d.services.rawLogger, d.services.p.window.get()},
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

    auto asyncFs = std::make_unique<StdAsyncFilesystem>(d.services.assetsRoot, d.services.userDataDir);
    if (!asyncFs->init()) {
        d.services.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, asyncFs->getLastError());
        return false;
    }
    d.services.p.asyncFilesystem = std::move(asyncFs);

    // HTTP client (#490) for content downloads — null when built without libcurl (lean/dev builds);
    // service()d each frame beside the async filesystem. The first-run content flow consumes it.
    d.services.p.httpClient = createHttpClient(d.services.rawLogger);
    if (d.services.p.httpClient && !d.services.p.httpClient->init()) {
        d.services.rawLogger->log(LogLevel::Warn, __FILE__, __LINE__, "HTTP client init failed; downloads disabled");
        d.services.p.httpClient.reset();
    }

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

    ModLoader modLoader(*d.services.p.filesystem, *d.services.rawLogger, d.services.assetsRoot.string());
    auto packs = modLoader.load();
    const bool hasPacks = !packs.empty();

    // Report the pack count on startup the way fl-server does (#831). The client logs to a file the
    // developer has no reason to open, so also echo one line to stderr: a client drawing placeholder
    // tetrahedra with "content: 0 mod(s) loaded" here is an assets-root problem, not a broken mesh.
    {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "content: %zu mod(s) loaded from %s", packs.size(),
                      d.services.assetsRoot.string().c_str());
        d.services.rawLogger->log(LogLevel::Info, __FILE__, __LINE__, buf);
        std::fprintf(stderr, "%s\n", buf);
    }

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

    // Mount the bundled coarse global base (#474) at lowest priority so a zero-user-pack launch
    // still gets real-Earth root tiles (generateProceduralTile fills finer detail; user packs
    // override). No-op when no base is bundled. Appended after hasPacks/crash-list so those still
    // reflect user packs only.
    if (auto base = fl::loadBundledBaseTerrain(*d.services.p.filesystem, *d.services.rawLogger, "base-terrain",
                                               fl::builtinWorldTerrainManifest().terrainId))
        packs.push_back(std::move(base));

    d.services.assets = std::make_unique<AssetManager>(std::move(packs), *d.services.rawLogger);
    d.services.assets->initialize(d.services.p.window.get());

    // The client resolves def ids the same way the server does (#810) -- it needs to, because the
    // in-flight manual (#821) reads the aircraft's stations and sensors out of the same content pack.
    {
        static constexpr fl::AssetType kIndexedTypes[] = {fl::AssetType::EntityDef, fl::AssetType::SensorDef,
                                                          fl::AssetType::Weapon};
        d.services.contentIndex.build(*d.services.assets, kIndexedTypes, *d.services.rawLogger);
        // Builtins first (#440), mirroring fl-server: the sandbox debug entity's stores must
        // resolve to NAMES for the HUD weapon line even with zero packs mounted.
        fl::registerBuiltinWeapons(d.services.weapons);
        fl::registerPackWeaponDefs(*d.services.assets, d.services.weapons, *d.services.rawLogger);
    }

    FirstRun firstRun(*d.services.userConfig, *d.services.rawLogger);
    d.services.outcome = firstRun.check(hasPacks);

    return true;
}

// Generates the in-flight aircraft manual for the type we are flying (#821).
//
// THE CLIENT READS THE PACK HERE, AND THAT IS CORRECT. #811 deleted a disk re-load of the entity def
// because it was WRONG -- it looked the def up by a namespaced id that was never a filename, missed,
// and silently flew the builtin model. Reading the pack is not the sin; reading it by the wrong key
// was. ContentIndex (#810) makes the id resolvable, and a manual is exactly the kind of thing the
// client should build from its own content: it is a reference card, not physics, and the sim's
// authority is untouched.
//
// A client that does not have the pack (a joiner without the content) simply gets the sections it can
// build. Nothing here is load-bearing for flight.
void Game::buildManualFor(uint32_t typeIndex) {
    auto& d = *m_impl;

    const fl::EntityDef* wireDef = d.services.entityRegistry.byIndex(typeIndex);
    if (!wireDef || !d.services.assets)
        return;

    // The wire carries id, mesh, flight model and the payload floats -- not hardpoints, and not the
    // sensor list (#811 deliberately did not over-deliver). Resolve the FULL def from the pack.
    fl::EntityDef fullDef = *wireDef;
    if (const std::string* asset = d.services.contentIndex.assetNameFor(fl::AssetType::EntityDef, wireDef->id)) {
        if (auto raw = d.services.assets->loadEntityDef(asset->c_str()); raw && !raw->bytes.empty()) {
            try {
                fullDef = fl::parseEntityDef(
                    std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size()));
            } catch (const std::exception& e) {
                d.services.rawLogger->log(
                    fl::LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("manual: entity def '") + wireDef->id + "' failed to parse: " + e.what()).c_str());
            }
        }
    }
    // Zero-pack sandbox: the wire def has no hardpoints and there is no pack to resolve them from,
    // but the builtin aircraft ARE armed (#440/#977) — use the same builders the server spawned from.
    if (fullDef.hardpoints.empty() && wireDef->id == "builtin:debug-entity")
        fullDef = fl::builtinDebugEntityDef();
    else if (fullDef.crew.empty() && wireDef->id == "builtin:bomber")
        fullDef = fl::builtinBomberDef(); // rebuild the crewed def so the manual can render its crew page

    // Weapon-station glue (#440): the selector needs the station count, the HUD weapon line needs
    // names. Wired here — the one place the client resolves the full def of the aircraft it flies —
    // and BEFORE the flight-model early-out below, so a joiner without the pack's flight model
    // still gets a working selector. A client with no def (no pack, non-builtin type) gets
    // stationCount 0: cycling is off and the HUD line stays blank, by design.
    d.services.flightInput.setStationCount(static_cast<uint8_t>(std::min<std::size_t>(fullDef.hardpoints.size(), 254)));
    {
        std::vector<std::string> labels;
        labels.reserve(fullDef.hardpoints.size());
        for (const fl::Hardpoint& hpt : fullDef.hardpoints) {
            const fl::WeaponDef* w =
                hpt.defaultWeapon.empty() ? nullptr : d.services.weapons.findById(hpt.defaultWeapon.c_str());
            labels.push_back(w ? w->name : hpt.defaultWeapon); // unresolved id shown verbatim; empty = empty
        }
        d.services.flightHud.setStationLabels(std::move(labels));
    }

    // The flight model: the same one prediction flies, resolved the same way.
    auto resolver = fl::makeFlightModelResolver(d.services.entityRegistry, *d.services.assets, *d.services.rawLogger);
    std::shared_ptr<const fl::FlightModelData> model = resolver(typeIndex);
    if (!model)
        return;

    // Sensors, by id, through the index (#810).
    std::vector<std::shared_ptr<const fl::sensor::SensorDef>> owned;
    fl::ManualSources src;
    for (const std::string& id : fullDef.sensorIds) {
        const std::string* asset = d.services.contentIndex.assetNameFor(fl::AssetType::SensorDef, id);
        if (!asset)
            continue;
        auto raw = d.services.assets->loadSensorDef(asset->c_str());
        if (!raw || raw->bytes.empty())
            continue;
        try {
            owned.push_back(std::make_shared<const fl::sensor::SensorDef>(fl::sensor::parseSensorDef(
                std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size()))));
        } catch (const std::exception&) {
            // A sensor that will not parse is reported by the server's resolver; the manual just
            // leaves it out rather than duplicating the complaint.
        }
    }
    for (const auto& s : owned)
        src.sensors.push_back(s.get());

    // The prose -- the ONLY hand-written part of the manual, and it contains no numbers.
    std::string prose;
    if (!fullDef.manualAsset.empty()) {
        if (auto raw = d.services.assets->loadManualProse(fullDef.manualAsset.c_str()); raw && !raw->bytes.empty())
            prose.assign(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size());
    }

    src.entity = &fullDef;
    src.model = model.get();
    src.weapons = &d.services.weapons;
    // The payload the SERVER computed and sent (#812), not one the client re-derived -- so the manual
    // quotes the loadout the aircraft is actually flying with.
    src.payload = fl::PayloadEffect{wireDef->payloadMassKg, wireDef->payloadCd0};
    src.prose = std::move(prose);

    d.services.manual.setManual(fl::buildAircraftManual(src));
}

// Steps 17–17d: entity registry, scene renderer, particle system, terrain, audio systems, sandbox.
void Game::initGameSystems() {
    auto& d = *m_impl;

    fl::registerBuiltinParticlePresets(d.services.particleSystem);

    d.services.sceneRenderer = std::make_unique<fl::SceneRenderer>(
        d.services.renderBridge,
        [&reg = d.services.entityRegistry](uint32_t idx, fl::SceneRenderer::ResolvedMesh& out) -> bool {
            const fl::EntityDef* def = reg.byIndex(idx);
            if (!def)
                return false;
            out.meshName = def->mesh;
            out.damageMeshName = def->classicDamageMesh;
            // Category + projectile kind arrive on MsgEntityTypeDef (#886); a mesh-less type
            // renders as its category's builtin placeholder silhouette.
            out.shape = fl::builtinShapeFor(def->category, def->projectileKind);
            return true;
        },
        *d.services.assets, *d.services.p.renderer);
    d.services.sceneRenderer->setDrawDistance(d.services.rendererSettings.drawDistanceKm);
    d.services.sceneRenderer->setLogger(d.services.rawLogger);

    d.services.terrainStreamer =
        std::make_unique<fl::TerrainStreamer>(fl::builtinWorldTerrainManifest(), *d.services.assets,
                                              *d.services.p.asyncFilesystem, d.services.p.renderer.get());
    d.services.sceneRenderer->setTerrainStreamer(d.services.terrainStreamer.get());
    // Drop last session's airport registry so the next MsgConnectAck rebuilds it against the fresh
    // terrain streamer + this session's planet radius (#486; the height modifier is re-wired there).
    d.services.airportRegistry.reset();

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

    // Livery indirection (#845): re-skin an aircraft by material slot without touching its geometry.
    // Resolve the highest-priority installed livery targeting the entity's aircraft DEF ID; the
    // SceneRenderer caches the result per type and applies the "<slot>.<map>" texture overrides with
    // per-map fallback to the base scheme. No livery installed -> the aircraft flies its base textures.
    d.services.sceneRenderer->setLiveryResolver([&reg = d.services.entityRegistry, assets = d.services.assets.get()](
                                                    uint32_t idx, fl::SceneRenderer::LiveryTextureSet& out) -> bool {
        const fl::EntityDef* def = reg.byIndex(idx);
        if (!def)
            return false;
        auto livery = assets->liveryForAircraft(def->id.c_str());
        if (!livery)
            return false;
        out.id = livery->id;
        out.overrides = std::move(livery->textures);
        return true;
    });

    d.services.subtitleQueue.setEnabled(d.services.userConfig->accessibility().subtitlesEnabled);
    d.services.sceneRenderer->setSubtitleQueue(&d.services.subtitleQueue);

    if (d.services.musicManager.init(d.services.p.audio.get(), d.services.assets.get(), d.services.rawLogger)) {
        // A pack playlist.toml wins; with none, fall back to the builtin procedural playlist (#865) so
        // menus and flight still have music with zero content mounted.
        auto playlistText = d.services.assets->loadConfig("playlist.toml");
        PlaylistData playlist =
            playlistText ? parsePlaylist(*playlistText, *d.services.rawLogger) : builtinDefaultPlaylist();
        d.services.musicManager.loadPlaylist(playlist);
        d.services.musicManager.setState(GameState::Menu);
    }

    // Weapon SFX (#631): the fire path's audio. A null audio device (rare) is tolerated — the
    // manager no-ops. Presets map the wire effect vocabulary to a pack asset (overridable) with a
    // compiled-in procedural fallback, so the guns are audible with zero content mounted.
    d.services.sfxManager.init(d.services.p.audio.get(), d.services.assets.get(), d.services.rawLogger);
    fl::registerBuiltinSfxPresets(d.services.sfxManager);
    // Warning tones (#957): stall / overspeed cockpit cues. Null audio device tolerated (no-op).
    d.services.warningTones.init(d.services.p.audio.get(), d.services.rawLogger);
    // Continuous engine + aero doppler layers (#959): own-ship head-locked engine/wind + positional
    // flyby engines. Only air vehicles hum; the predicate reads the type registry's category.
    d.services.engineAudio.init(d.services.p.audio.get(), d.services.rawLogger);
    d.services.engineAudio.setAirVehiclePredicate([&reg = d.services.entityRegistry](uint32_t typeIndex) {
        const fl::EntityDef* def = reg.byIndex(typeIndex);
        return def && (def->category == fl::ObjectCategory::AirVehicle || def->category == fl::ObjectCategory::Player);
    });
    // Radio voice callouts (#704): the server sends resolved subtitle TEXT (not a key), so no
    // Localization/synth is wired — playText pushes the subtitle and plays a pack OGG if one exists,
    // else it degrades to a text-only subtitle. Null audio device tolerated.
    d.services.voiceCallouts.init(d.services.p.audio.get(), d.services.assets.get(), &d.services.subtitleQueue,
                                  /*i18n=*/nullptr, d.services.rawLogger, /*synth=*/nullptr);
    d.services.audioSettings = d.services.userConfig->audio();
    d.services.effectRouter.setSfx(&d.services.sfxManager, &d.services.audioSettings);
}

// Steps 19–20: debug console — console widget only; server commands wired in startGame().
void Game::initGameConsole() {
    auto& d = *m_impl;
    d.services.gameConsole.emplace(*d.services.rawLogger, d.services.cmdRegistry);
}

// Step 21: screen manager — created after all stable game systems exist.
void Game::initScreenManager() {
    auto& d = *m_impl;
    d.services.screenMgr = std::make_unique<ScreenManager>(*d.services.p.input);
    d.services.screenMgr->init(*d.services.userConfig, *d.services.p.renderer, *d.services.p.window,
                               *d.services.p.display, *d.services.assets, !d.services.connectHost.empty());
}

// ---------------------------------------------------------------------------
// Session lifecycle — startGame / stopGame
// ---------------------------------------------------------------------------

void Game::startGame(const std::string& mission) {
    auto& d = *m_impl;

    // Reset render bridge and entity registry from any prior session.
    d.services.renderBridge.reset();
    d.services.entityRegistry.clear();
    d.services.camInput.startSession();
    d.services.env = EnvironmentState{};
    d.session.serverReady.store(false, std::memory_order_relaxed);
    d.session.sessionFailure.store(SessionFailure::None, std::memory_order_relaxed);
    d.session.sessionStart = std::chrono::steady_clock::now(); // flight-time accrual at debrief (#634)

    // The camera pose is recomputed from the entity snapshot every Flight frame (CameraInput runs
    // before the render, and 3D rendering is skipped during the loading overlay), so no stale-state
    // reset is needed here.

    // Register the builtin entity type for the no-pack sandbox path — ARMED (#440), via the same
    // builder fl-server uses, so the client-side hardpoint count matches what the server spawned.
    if (d.services.outcome == FirstRunOutcome::LaunchSandboxInspector) {
        d.services.entityRegistry.registerType(fl::builtinDebugEntityDef());
        d.services.entityRegistry.registerType(fl::builtinBomberDef()); // the crewed bomber (#977)
    }

    const bool isMultiplayer = !d.services.connectHost.empty();

    if (!isMultiplayer) {
        // Single-player: spawn fl-server subprocess in a background thread.
        d.session.localServer.emplace(*d.services.rawLogger);
        // Forward the client's resolved content root so the embedded server loads the same packs (#831).
        d.session.localServer->setContentRoot(d.services.assetsRoot.string());
        // Load the chosen mission (#634); empty = the sandbox, as before.
        d.session.localServer->setMission(mission);
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
        d.services.effectRouter.setHaptics(&*d.session.hapticController); // own-ship launch/release feedback (#631)

        // Single-player uses enet6 to match the embedded LocalServer (spawned with --transport enet);
        // multiplayer joins a dedicated server over GNS (encrypted). Both via the HAL factory.
        const TransportKind clientTransport = isMultiplayer ? TransportKind::Gns : TransportKind::Enet;
        d.session.clientNet = createNetwork(clientTransport, d.services.rawLogger);
        if (!d.session.clientNet->init()) {
            d.services.rawLogger->log(LogLevel::Error, __FILE__, __LINE__, "client network init failed");
            return;
        }

        d.session.clientHandler =
            std::make_unique<ClientNetEventHandler>(d.services.renderBridge, d.services.entityRegistry,
                                                    *d.services.rawLogger, *d.session.clientNet, d.services.env);
        d.session.clientHandler->notice = &d.services.serverNotice;
        d.session.clientHandler->wingman = &d.services.wingmanMenu; // check-ins, order acks, relayed calls
        d.session.clientHandler->console = &*d.services.gameConsole;
        d.session.clientHandler->effects = &d.services.effectRouter; // weapon cosmetics (#625)
        d.services.effectRouter.reset();                             // no stale effects across sessions
        // ATC/radio callouts (#704): render each transmission as a subtitle (+ a pack OGG at
        // radio/<voiceKey> if one exists, else text-only). The console already logged the raw line.
        d.session.clientHandler->radioCallback = [&d](const char* speaker, const char* text, const char* voiceKey,
                                                      uint16_t seconds) {
            std::string line =
                (speaker && speaker[0] != '\0') ? std::string(speaker) + ": " + (text ? text : "") : (text ? text : "");
            std::string asset;
            if (voiceKey && voiceKey[0] != '\0')
                asset = std::string("radio/") + voiceKey;
            d.services.voiceCallouts.playText(line, asset.empty() ? nullptr : asset.c_str(),
                                              seconds > 0 ? static_cast<float>(seconds) : 4.f,
                                              d.services.audioSettings);
        };
        // Server-driven music (#413/#166): a mission/AI world.set_music_state() reaches MusicManager here.
        d.session.clientHandler->musicStateCallback = [&d](uint8_t state) {
            if (fl::isGameStateOrdinal(state))
                d.services.musicManager.setState(static_cast<fl::GameState>(state));
        };
        // Scripted haptics (#128): a Lua rumble()/rumble_triggers()/stop_rumble() plays on the local
        // (current-player) gamepad 0. Args are already clamped server-side by the engine binding.
        d.session.clientHandler->hapticCallback = [&d](uint8_t kind, float a, float b, uint16_t durMs) {
            fl::IInput& in = *d.services.p.input;
            switch (static_cast<fl::HapticKind>(kind)) {
            case fl::HapticKind::Rumble:
                in.rumble(0, a, b, durMs);
                break;
            case fl::HapticKind::Triggers:
                in.rumbleTriggers(0, a, b, durMs);
                break;
            case fl::HapticKind::Stop:
                in.stopRumble(0);
                break;
            }
        };
        d.session.clientHandler->motdDisplaySeconds = d.services.userConfig->client().motdDisplayS;
        d.session.clientHandler->sessionFailure = &d.session.sessionFailure;
        // Connect-handshake inputs (#853/#834/#857): request a specific aircraft if --aircraft was given
        // (empty = let the server pick its [world] player_entity_type default), and the observer role if
        // --observer was given (a spectator with no aircraft).
        d.session.clientHandler->requestedEntityType = d.services.requestedEntityType;
        d.session.clientHandler->requestedRole =
            d.services.requestObserver ? fl::PeerRole::Observer : fl::PeerRole::Pilot;
        // Player callsign for the match roster (#996), from the pilot profile.
        d.session.clientHandler->requestedCallsign = d.services.userConfig->pilot().profile.callsign;
        // Client identity for reconnect (#524) + optional join password (#998).
        d.session.clientHandler->requestedGuid = d.services.userConfig->pilot().profile.guid;
        d.session.clientHandler->requestedJoinPassword = d.services.joinPassword;
        // Report the client's mounted content packs so the server can enforce its required-pack policy
        // (#872 wire half). Warn-only server-side for now; content hashing is a later addition.
        d.session.clientHandler->packManifest.clear();
        if (d.services.assets) {
            for (const auto& p : d.services.assets->packManifest()) {
                fl::PackManifestEntry e{};
                std::snprintf(e.id, sizeof(e.id), "%s", p.id.c_str());
                std::snprintf(e.version, sizeof(e.version), "%s", p.version.c_str());
                d.session.clientHandler->packManifest.push_back(e);
            }
        }
        d.session.clientNet->setEventHandler(d.session.clientHandler.get());

        if (!isMultiplayer) {
            d.services.env = d.session.localServer->initialEnvironment();

            auto adminSender =
                makeNetworkAdminSender(*d.session.clientNet, std::string(d.session.localServer->sessionToken()));
            d.session.localServer->registerConsoleCommands(
                d.services.cmdRegistry, adminSender, d.services.renderBridge, &d.services.entityRegistry,
                &d.session.clientHandler->assignedEntityIdx, &d.session.clientHandler->assignedEntityGen,
                &d.services.gameConsole->showPosRef(), &d.services.showPing);
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
            ctx.showPing = &d.services.showPing;
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
        fsd.clientNetHandler = d.session.clientHandler.get();
        fsd.entityRegistry = &d.services.entityRegistry;
        fsd.joystick = d.services.p.joystick.get();
        fsd.userConfig = &*d.services.userConfig;
        fsd.inspector = d.session.inspector ? &*d.session.inspector : nullptr;
        fsd.prediction = &d.services.prediction;
        fsd.wingmanMenu = &d.services.wingmanMenu;
        fsd.commsMenu = &d.services.commsMenu;
        fsd.manual = &d.services.manual;
        fsd.assignedEntityIdx = &d.session.clientHandler->assignedEntityIdx;
        fsd.assignedEntityGen = &d.session.clientHandler->assignedEntityGen;

        // NOTE: ClientPrediction::init() is deliberately NOT called here. onConnect runs
        // before clientNet->connect() below, so the player's entity idx/gen and the
        // server's planet radius are still the pre-MsgConnectAck defaults (0/0/6371).
        // Prediction is wired at the ConnectAck one-shot gate in the run loop instead
        // (#755); reinitFlight only stores live pointers, so it can stay here.
        d.services.screenMgr->reinitFlight(std::move(fsd));

        const char* host = isMultiplayer ? d.services.connectHost.c_str() : "127.0.0.1";
        const uint16_t port = isMultiplayer ? d.services.connectPort : uint16_t{4778};
        d.session.clientNet->connect(host, port);
    };

    // isConnected requires a snapshot, a processed ConnectAck (assignedEntityGen != 0),
    // at least one terrain chunk, AND the assigned entity present in the current
    // snapshot. ConnectAck (reliable ch0) and WorldSnapshot (unreliable ch1) arrive
    // on independent ENet channels: a snapshot can precede the ConnectAck, so the
    // first snapshot we see may not yet include the peer's entity. Waiting for the
    // entity to appear in the bridge guarantees findEntry() returns non-null on the
    // very first FlightScreen frame, so the camera is never stuck at the stale
    // default pivot (Y=2000 m) from startGame().
    d.services.screenMgr->reinitLoading(
        d.session.serverReady,
        [&d]() -> bool {
            if (!d.services.renderBridge.hasSnapshot() || !d.session.clientHandler)
                return false;
            if (!d.session.clientHandler->gotConnectAck() || d.services.terrainStreamer->tileCount() == 0)
                return false;
            // Observer (#857): no ownship entity to wait for — a decoded ack + snapshot + terrain is
            // enough. It free-flies a ghost camera; the full ghost UX is #859.
            if (d.session.clientHandler->grantedRole() == fl::PeerRole::Observer)
                return true;
            const uint32_t gen = d.session.clientHandler->assignedEntityGen;
            if (gen == 0)
                return false;
            const uint32_t idx = d.session.clientHandler->assignedEntityIdx;
            for (const auto& e : d.services.renderBridge.current().entries)
                if (e.entityIdx == idx && e.entityGen == gen)
                    return true;
            return false;
        },
        std::move(onConnect), !isMultiplayer, &d.session.sessionFailure);

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
    d.session.connectAckApplied = false;
    d.services.prediction.reset();
    d.services.renderBridge.reset();
    d.services.entityRegistry.clear();
    d.services.env = EnvironmentState{};
    d.services.musicManager.setState(GameState::Menu);
    d.services.screenMgr->setServerCmd(nullptr);
    d.services.p.input->setMouseCapture(false);
}

// The mission's display name (its `name:` field), for the brief screen. Loads and parses the mission
// asset via the shared engine-mission parser — the same runtime the server loads it with (#634/#632),
// so the brief and the flown mission cannot disagree. Falls back to the id when the mission is missing
// or unparseable.
static std::string missionDisplayName(AssetManager* assets, const std::string& id) {
    if (!assets || id.empty())
        return id;
    auto data = assets->loadMission(id.c_str());
    if (!data || data->bytes.empty())
        return id;
    const std::string yaml(data->bytes.begin(), data->bytes.end());
    const MissionParseResult r = parseMission(yaml);
    return (r.ok && !r.mission.name.empty()) ? r.mission.name : id;
}

void Game::handleTransition(Screen next) {
    auto& d = *m_impl;
    const Screen prev = d.services.screenMgr->current();

    // Carry the chosen mission from the select screen into the brief so it renders the real mission
    // (#634). The brief reads it back out when the player hits Fly.
    if (next == Screen::MissionBrief && prev == Screen::MissionSelect) {
        const std::string& id = d.services.screenMgr->missionSelect().selectedMission();
        d.services.screenMgr->missionBrief().setMission(id, missionDisplayName(d.services.assets.get(), id));
    }

    // Start a session on ANY entry into Loading, not just from MainMenu — the mission flow enters
    // Loading from MissionBrief, which the old MainMenu-only guard missed, leaving the LoadingScreen
    // null and crashing the next frame (#876). Entering from the brief loads the chosen pack mission;
    // entering from the menu loads the item's mission — "builtin:sandbox" for Instant Action (#40), empty
    // for Free Flight / Join Server.
    if (entersSession(prev, next)) {
        const std::string mission = (prev == Screen::MissionBrief)
                                        ? d.services.screenMgr->missionSelect().selectedMission()
                                        : d.services.screenMgr->mainMenu().confirmedMission();
        startGame(mission);
    }

    if (exitsSession(prev, next))
        stopGame();

    if (next == Screen::Flight) {
        d.services.musicManager.setState(GameState::FlightPatrol);
        // Planet radius from MsgConnectAck is applied in the per-frame terrain block in
        // run() as soon as the ConnectAck is processed — before any tile is streamed.
    } else if (next == Screen::MainMenu)
        d.services.musicManager.setState(GameState::Menu);
    else if (next == Screen::Debrief) {
        // Real session stats (#626): the server's tallies, delivered on the CombatEvent channel.
        uint32_t kills = 0;
        uint32_t losses = 0;
        if (d.session.clientHandler) {
            kills = d.session.clientHandler->sessionStats().kills;
            losses = d.session.clientHandler->sessionStats().losses;
        }
        // Real mission outcome (#584): the objective evaluator's result, delivered on MsgMissionOutcome.
        // A session with no mission (Free Flight) never sends one, so the default reads as success.
        const bool missionSuccess =
            !d.session.clientHandler || d.session.clientHandler->missionOutcome() != fl::MissionResultCode::Failure;
        d.services.screenMgr->debrief().setStats(static_cast<int>(kills), static_cast<int>(losses), missionSuccess);
        // The pilot's career log accumulates per session, exactly once, on the way into debrief:
        // kills/losses from the server's tallies plus this session's flight time (#634).
        if (d.services.userConfig) {
            const auto elapsed = std::chrono::steady_clock::now() - d.session.sessionStart;
            const int64_t flightSecs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            PilotSettings ps = d.services.userConfig->pilot();
            ps.profile.kills += static_cast<int>(kills);
            ps.profile.losses += static_cast<int>(losses);
            ps.profile.flightTimeS += flightSecs;
            // Pilot logbook (#674): the career record accrues the same debrief deltas. Per-class kills
            // come from the classified kill feed; the mission counts as flown (and failed when the
            // objective evaluator said so, #584). Ejections (#672) are recorded on their own path.
            if (d.session.clientHandler) {
                const auto& s = d.session.clientHandler->sessionStats();
                uint32_t classified = 0;
                for (int c = 0; c < 8; ++c) {
                    for (uint32_t i = 0; i < s.killsByClass[c]; ++i)
                        ps.profile.logbook.recordKill(c);
                    classified += s.killsByClass[c];
                }
                // Any kills the feed could not classify still count (as the default air class), so the
                // logbook's total matches the debrief's kill delta exactly.
                for (uint32_t i = classified; i < kills; ++i)
                    ps.profile.logbook.recordKill(0);
            }
            ps.profile.logbook.recordMission(missionSuccess);
            d.services.userConfig->setPilot(ps);
        }
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

    // Auto-start (menu bypass): enter the session before the first frame through the exact path a
    // menu confirm takes — set the confirmed mission and fire the enters-session transition — so
    // LoadingScreen construction, server spawn, and connect wiring are identical to a human Enter.
    if (d.services.autoStart) {
        d.services.screenMgr->mainMenu().setConfirmedMission(d.services.autoStartMission);
        handleTransition(Screen::Loading);
    }

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

        // If a snapshot is available, advance the bridge and prime CameraInput's render
        // alpha BEFORE the screen update so that CameraInput::update() (called from
        // FlightScreen::update below) extrapolates the camera target by the same
        // velocity × alpha × kTickDt that SceneRenderer uses for entity positions.
        CameraView cam{};
        glm::dvec3 camOrigin{};
        const fl::EntityRenderEntry* playerEntry = nullptr;
        float alpha = 0.f;
        float aspect = 1.f;
        if (inSession && d.session.clientHandler && d.services.renderBridge.hasSnapshot()) {
            d.services.renderBridge.tryAdvance(); // consume latest snapshot before screen update
            playerEntry = findPlayerEntry(d.services.renderBridge, d.session.clientHandler->assignedEntityIdx,
                                          d.session.clientHandler->assignedEntityGen);
            alpha = d.session.clientHandler->tickAlpha.get();
            aspect = static_cast<float>(d.services.p.window->width()) /
                     static_cast<float>(d.services.p.window->height() > 0 ? d.services.p.window->height() : 1);
            d.services.camInput.setRenderAlpha(alpha);

            // Generate the aircraft manual once, as soon as we know which aircraft we are flying
            // (#821). buildAircraftManual trims the flight model, which is a root-finding loop --
            // it belongs here, once, and never in the render path.
            if (playerEntry && !d.session.manualBuilt) {
                d.session.manualBuilt = true;
                buildManualFor(playerEntry->typeIndex);
            }
        }

        // Service terrain and async I/O every session frame, before the screen update,
        // so tileCount() reflects the current state when isConnected() is evaluated
        // inside LoadingScreen. For procedural terrain this makes tiles available in
        // the same frame they are requested. For content-pack PNG tiles, reads progress
        // before the transition check runs. Streaming waits for the server's
        // MsgConnectAck (assignedEntityGen != 0): tiles bake the planet radius at
        // generation time, so updating earlier would bake Earth-radius tiles during
        // Loading and immediately invalidate them on a non-Earth server.
        // Drain HTTP completions every frame (content downloads run at the menu too, not just
        // in-session). Null when built without libcurl (#490).
        if (d.services.p.httpClient)
            d.services.p.httpClient->service();

        if (inSession) {
            d.services.p.asyncFilesystem->service();
            // A pilot is "ready" once its ConnectAck assigns an entity (gen != 0). An observer (#859)
            // never gets an entity, so its readiness is just that the ConnectAck arrived — it still
            // needs the planet radius applied and terrain streamed, only keyed on its ghost camera.
            const bool observer =
                d.session.clientHandler && d.session.clientHandler->grantedRole() == fl::PeerRole::Observer;
            const bool ackReady = d.session.clientHandler && (d.session.clientHandler->assignedEntityGen != 0 ||
                                                              (observer && d.session.clientHandler->gotConnectAck()));
            if (ackReady) {
                // Re-run the setup when a mid-session role switch (#859) re-sends MsgConnectAck with a
                // different entity/role — otherwise the one-shot flag would strand a switched peer with
                // stale prediction/camera state.
                const uint32_t ackGen = d.session.clientHandler->assignedEntityGen;
                const fl::PeerRole ackRole = d.session.clientHandler->grantedRole();
                const bool ackChanged = d.session.connectAckApplied &&
                                        (ackGen != d.session.appliedEntityGen || ackRole != d.session.appliedRole);
                if (!d.session.connectAckApplied || ackChanged) {
                    const double radiusM = static_cast<double>(d.session.clientHandler->planetRadiusKm()) * 1000.0;
                    d.services.terrainStreamer->setPlanetRadius(radiusM);
                    // Camera "up" = radial direction on the planet, so the horizon stays
                    // level far from the origin.
                    d.services.camInput.setPlanetRadius(radiusM);

                    // Airport registry + runway terrain flattening (#486). Built once per session from
                    // the SAME bundled data the server used (builtin airfield + pack airports + the
                    // OurAirports CSV), at the server's planet radius, so heightAt() flattens
                    // identically on both ends — the physics floor the client predicts and the terrain
                    // it renders both match the server's. All CSV airports carry explicit elevations
                    // and the builtin airfield is fixed, so no terrain priming is needed at load.
                    if (!d.services.airportRegistry) {
                        d.services.airportRegistry = std::make_unique<fl::AirportRegistry>();
                        std::vector<fl::AirportDef> airportDefs;
                        airportDefs.push_back(fl::builtinAirfield());
                        fl::registerPackAirportDefs(*d.services.assets, airportDefs, *d.services.p.logger);
                        for (auto& ad : fl::loadOrImportAirports(*d.services.p.filesystem, *d.services.p.logger))
                            airportDefs.push_back(std::move(ad));
                        auto* terrain = d.services.terrainStreamer.get();
                        d.services.airportRegistry->load(std::move(airportDefs), radiusM,
                                                         [terrain](glm::dvec3 pos) { return terrain->heightAt(pos); });
                        fl::AirportRegistry* reg = d.services.airportRegistry.get();
                        d.services.terrainStreamer->setHeightModifier(
                            [reg](glm::dvec3 pos, double rawH) { return reg->flattenedHeight(pos, rawH); },
                            [reg](glm::dvec3 centre, double radM) { return reg->regionHasRunway(centre, radM); });
                        // Runway surface typing (#487): the same override the server uses, so the
                        // rendered runway and the predicted rollout agree on the surface.
                        d.services.terrainStreamer->setSurfaceOverride(
                            [reg](glm::dvec3 pos) -> std::optional<fl::SurfaceType> {
                                const auto s = reg->runwaySurfaceAt(pos);
                                return s ? std::optional<fl::SurfaceType>(fl::surfaceTypeForRunway(*s)) : std::nullopt;
                            });
                        // Wire the airport registry into the scene renderer so it draws the runways.
                        d.services.sceneRenderer->setAirportRegistry(reg);
                    }

                    // A gunner (#975) occupies a NON-fly seat of an aircraft it does not fly, so it must
                    // NOT run flight prediction — only the Fly seat predicts. It still views the airframe
                    // (assignedEntity = the host), so it keeps the cockpit camera rather than the ghost
                    // free-fly, but its predictor is torn down like an observer's.
                    const bool inCrewSeat = d.session.clientHandler->inCrewSeat();
                    if (observer) {
                        // Ghost camera (#859): there is no ownship to predict, so ClientPrediction is
                        // torn down (a pilot→observer switch leaves a stale predictor otherwise). Force
                        // the free-fly camera — Cockpit/Chase render nothing without a player entity —
                        // and place the eye at an overview vantage above the origin; WASD flies it from
                        // there, and the eye we send drives server-side interest (#858).
                        d.services.prediction.reset();
                        d.services.cameraController.setMode(fl::CameraMode::Free);
                        d.services.camInput.setFlyEye(glm::dvec3{0.0, 3000.0, 0.0});
                    } else if (inCrewSeat) {
                        // Gunner seat: no flight prediction, but keep viewing the host airframe.
                        d.services.prediction.reset();
                    } else {
                        // Becoming a pilot mid-session (observer→pilot switch): drop the ghost's free
                        // camera into the cockpit of the freshly assigned aircraft. Harmless on the
                        // initial pilot connect, where the mode already defaults to Cockpit.
                        if (ackChanged)
                            d.services.cameraController.setMode(fl::CameraMode::Cockpit);
                        // Wire client-side prediction now that MsgConnectAck has populated the
                        // player's entity idx/gen and the server's planet radius. Doing this in
                        // onConnect (before connect()) captured the pre-ack defaults (0/0/6371),
                        // leaving reconcile() a permanent no-op (#755). The resolver captures only
                        // &d (session-lived services), so building it here is equivalent. A re-init
                        // (respawn / role switch) overwrites the prior predictor with the new idx/gen.
                        // The entity's flight model now arrives on MsgEntityTypeDef (#811), so the client
                        // reads the field instead of re-deriving it from disk by an id that was never a
                        // filename. Every fallback to the builtin model is logged at Error and names the id.
                        auto flightModelResolver = fl::makeFlightModelResolver(
                            d.services.entityRegistry, *d.services.assets, *d.services.p.logger);
                        // The default loadout's mass and drag, straight off MsgEntityTypeDef (#812) --
                        // the client carries the same stores as the server without needing a weapon
                        // registry to work out what they weigh.
                        auto payloadResolver = [&d](uint32_t typeIndex) -> fl::PayloadEffect {
                            const fl::EntityDef* def = d.services.entityRegistry.byIndex(typeIndex);
                            if (!def)
                                return {};
                            return fl::PayloadEffect{def->payloadMassKg, def->payloadCd0};
                        };
                        // The prediction ground floor composes terrain with any MOVING flight deck
                        // (#38), exactly as the server does — without this a carrier landing
                        // predicts straight through the deck and hard-snaps on reconcile. Deck
                        // footprints arrive on MsgEntityTypeDef; ship poses come from the latest
                        // render snapshot (main thread, same thread reconcile runs on).
                        auto heightQuery = [&d](glm::dvec3 pos) -> float {
                            float floorElev = d.services.terrainStreamer
                                                  ? static_cast<float>(d.services.terrainStreamer->heightAt(pos))
                                                  : 0.f;
                            const double posArr[3] = {pos.x, pos.y, pos.z};
                            const double planetR = d.session.clientHandler
                                                       ? double(d.session.clientHandler->planetRadiusKm()) * 1000.0
                                                       : fl::kEarthRadiusM;
                            const fl::RenderSnapshot& snap = d.services.renderBridge.current();
                            for (const fl::EntityRenderEntry& e : snap.entries) {
                                const fl::EntityDef* def = d.services.entityRegistry.byIndex(e.typeIndex);
                                if (!def || !def->deck)
                                    continue;
                                const double shipPos[3] = {e.position.x, e.position.y, e.position.z};
                                const float shipQuat[4] = {e.orientation.x, e.orientation.y, e.orientation.z,
                                                           e.orientation.w};
                                const fl::DeckLocalPoint lp = fl::deckLocalPoint(posArr, shipPos, shipQuat, *def->deck);
                                if (!fl::deckFloorApplies(lp, *def->deck))
                                    continue;
                                const double shipAlt =
                                    fl::geodeticAltitude(shipPos[0], shipPos[1], shipPos[2], planetR);
                                floorElev = std::max(floorElev, static_cast<float>(shipAlt + def->deck->heightM));
                            }
                            return floorElev;
                        };
                        d.services.prediction.init(d.services.userConfig->prediction(), std::move(flightModelResolver),
                                                   std::move(payloadResolver), std::move(heightQuery),
                                                   d.session.clientHandler->assignedEntityIdx,
                                                   d.session.clientHandler->assignedEntityGen,
                                                   d.session.clientHandler->planetRadiusKm());
                        // Per-surface rolling resistance in prediction (#487): the same
                        // TerrainStreamer surfaceTypeAt (with the runway override) the server reads,
                        // so a rollout on grass vs concrete predicts in parity.
                        d.services.prediction.setSurfaceQuery([&d](glm::dvec3 pos) {
                            return d.services.terrainStreamer ? d.services.terrainStreamer->surfaceTypeAt(pos)
                                                              : fl::SurfaceType::Unknown;
                        });
                    }

                    d.session.connectAckApplied = true;
                    d.session.appliedEntityGen = ackGen;
                    d.session.appliedRole = ackRole;
                }
                // Real screen height + FOV for the SSE refinement metric (window is resizable;
                // fovY matches CameraController's 60 deg default).
                d.services.terrainStreamer->setViewParams(static_cast<float>(d.services.p.window->height()),
                                                          glm::radians(60.0f));
                // Stream terrain around the ownship (pilot) or the ghost camera eye (observer, #859).
                const glm::dvec3 terrainPos =
                    playerEntry ? playerEntry->position : (observer ? d.services.camInput.eyeWorld() : glm::dvec3{});
                d.services.terrainStreamer->update(terrainPos);
            }
        }

        // Audio update (always — so music plays on main menu too).
        {
            const AudioSettings& aud = d.services.userConfig->audio();
            d.services.subtitleQueue.update(1.0f / 60.0f);
            d.services.musicManager.update(1.0f / 60.0f, aud.masterVolume, aud.musicVolume);

            // Warning tones (#957): drive stall/overspeed cues from the OWN aircraft's predicted
            // FlightState (the snapshot carries neither stalled nor Mach). predictedState() is null
            // off-session / for an observer, so inFlight stays false and the tones stay silent.
            fl::WarningToneInputs wt{};
            if (const fl::FlightState* st = d.services.prediction.predictedState()) {
                wt.inFlight = true;
                wt.stall = st->stalled;
                const double planetR =
                    static_cast<double>(d.session.clientHandler ? d.session.clientHandler->planetRadiusKm() : 6371.f) *
                    1000.0;
                const double altM = fl::geodeticAltitude(st->pos_world[0], st->pos_world[1], st->pos_world[2], planetR);
                const fl::AtmosphereState atmos = fl::computeAtmosphere(static_cast<float>(altM));
                const double spd = std::sqrt(st->vel_body[0] * st->vel_body[0] + st->vel_body[1] * st->vel_body[1] +
                                             st->vel_body[2] * st->vel_body[2]);
                const float maxMach = d.services.prediction.predictedMaxMach();
                wt.overspeed =
                    maxMach > 0.f && atmos.speed_of_sound_m_s > 0.f && (spd / atmos.speed_of_sound_m_s) > maxMach;
            }
            // RWR / missile-lock tones (#960): the worst HOSTILE threat level in this peer's legitimate
            // RWR picture (the datalink strobes the server decided it detects — no wallhack). Friendly
            // emitters are benign and never toned. Only meaningful for a pilot (inFlight).
            if (wt.inFlight && d.session.clientHandler) {
                const fl::RadarView rv = d.session.clientHandler->radarView();
                uint8_t worst = fl::kThreatSearch;
                bool any = false;
                for (const fl::RwrStrobe& s : rv.strobes) {
                    if (s.ident == fl::kIffFriend)
                        continue; // a friendly emitter is not a threat
                    any = true;
                    if (s.level > worst)
                        worst = s.level;
                }
                if (any)
                    wt.rwr = worst == fl::kThreatLaunch ? fl::RwrThreat::Launch
                             : worst == fl::kThreatLock ? fl::RwrThreat::Lock
                                                        : fl::RwrThreat::Search;
            }
            d.services.warningTones.update(wt, aud, 1.0f / 60.0f);
        }

        // Screen update — runs BEFORE camera computation so FlightScreen::update() →
        // CameraInput::update() sets the camera target from the current snapshot with
        // velocity extrapolation applied, making it coincident with the rendered entity.
        const Screen next = d.services.screenMgr->active().update(*d.services.p.input, *d.services.p.window);
        if (next == Screen::Quit) {
            if (inSession)
                stopGame();
            running = false;
        } else if (next != cur) {
            handleTransition(next);
        }

        // Null playerEntry if stopGame() reset the bridge mid-frame.
        if (!d.services.renderBridge.hasSnapshot())
            playerEntry = nullptr;

        // Render pipeline — camera is now set from the current snapshot by the screen
        // update above; renderFrame's internal tryAdvance() is a no-op this frame.
        // Skip 3D rendering during LoadingScreen: the loading overlay covers the viewport
        // and the camera has no valid entity target yet, so rendering would show stale
        // state (underground camera -> blue sky) bleeding through the overlay.
        if (inSession && cur != Screen::Loading && d.session.clientHandler && d.services.renderBridge.hasSnapshot()) {
            // Cinematic recorder (#916): override the camera with the shot-driven pose + per-shot FOV
            // before building the view. Only in Flight, once entities are streaming.
            if (d.services.recorder.active && cur == Screen::Flight) {
                driveRecorderCamera();
                cam = d.services.cameraController.view(aspect, glm::radians(d.services.recorder.curFovDeg), 0.1f);
            } else {
                cam = d.services.cameraController.view(aspect);
            }
            camOrigin = cam.worldOrigin;

            // Geographic sun (#481): the sun direction is PER-OBSERVER — derived from this camera's
            // own latitude/longitude (worldToGeodetic of the eye) and the shared UTC clock, so the
            // day/night terminator tracks longitude and two players far apart see different local
            // suns. This overwrites the legacy planar sun the weather packet seeded, and is recomputed
            // every frame because it depends on the (moving) camera position. Skipped until the first
            // weather packet supplies the UTC Julian Day.
            if (const double utcJd = d.session.clientHandler->utcJulianDay(); utcJd > 0.0) {
                const double planetR = static_cast<double>(d.session.clientHandler->planetRadiusKm()) * 1000.0;
                fl::WeatherController::applyGeographicSun(d.services.env, utcJd, cam.worldOrigin, planetR);
                // Night sky (#484): Moon + geographically-oriented stars for this observer. Must
                // follow the sun call (the night factor keys off env.sunDirection).
                fl::WeatherController::applyGeographicCelestial(d.services.env, utcJd, cam.worldOrigin, planetR);
            }

            updateAudioListener(*d.services.p.audio, cam, playerEntry ? playerEntry->velocity : glm::vec3{});

            // Weapon SFX (#631): the listener sits at the camera origin (sources are placed
            // camera-relative), and the router needs the origin + the own entity index to
            // spatialise effects and drive own-ship haptics. Set once per frame before the router
            // processes any effect.
            {
                const glm::vec3 fwd = -glm::vec3(cam.view[2][0], cam.view[2][1], cam.view[2][2]);
                const glm::vec3 up = glm::vec3(cam.view[1][0], cam.view[1][1], cam.view[1][2]);
                d.services.sfxManager.updateListener(fwd, up);
                d.services.effectRouter.setCameraOrigin(cam.worldOrigin);
                d.services.effectRouter.setOwnEntity(d.session.clientHandler->assignedEntityIdx);
            }

            // Continuous engine + aero doppler layers (#959): drive them from the current snapshot
            // AFTER the listener is at the origin (SfxManager::updateListener above). The ownship's
            // engine is head-locked; other air entities get positional doppler sources. An observer
            // (no own entity, assignedEntityGen == 0) still hears the flybys.
            {
                const uint32_t ownIdx = d.session.clientHandler->assignedEntityGen != 0
                                            ? d.session.clientHandler->assignedEntityIdx
                                            : fl::EngineAudioManager::kNoEntity;
                d.services.engineAudio.update(d.services.renderBridge.current().entries, ownIdx, cam.worldOrigin,
                                              d.services.userConfig->audio());
            }

            // In cockpit view the camera sits at the player entity, so render that entity
            // shadow-only — you should not see your own aircraft from inside it, but its shadow
            // on the ground should remain. External views (Chase/Free) show it normally.
            const bool cockpit = d.services.cameraController.mode() == fl::CameraMode::Cockpit;
            if (cockpit && playerEntry) {
                d.services.sceneRenderer->setHiddenEntity(d.session.clientHandler->assignedEntityIdx,
                                                          d.session.clientHandler->assignedEntityGen);
                // Cockpit interior (#870): render the ownship's cockpit mesh locked to the airframe.
                // Empty (the builtin debug entity has none yet — #852 G7) keeps the HUD-only cockpit.
                const fl::EntityDef* pdef = d.services.entityRegistry.byIndex(playerEntry->typeIndex);
                d.services.sceneRenderer->setCockpitMesh(pdef ? pdef->cockpitMesh : std::string{});
            } else {
                d.services.sceneRenderer->setHiddenEntity(0, 0);
                d.services.sceneRenderer->setCockpitMesh(""); // HUD-only in external views
            }

            // Merge precipitation with this frame's weapon effects (#625) into one emitter list.
            auto& emitters = d.services.frameEmitters;
            emitters.clear();
            const auto precip = d.services.precipController.build(d.services.env, cam, d.services.particleSystem);
            emitters.insert(emitters.end(), precip.begin(), precip.end());
            const auto fx = d.services.effectRouter.buildEmitters(d.services.particleSystem, 1.f / 60.f);
            emitters.insert(emitters.end(), fx.begin(), fx.end());
            d.services.sceneRenderer->renderFrame(alpha, cam, d.services.env, emitters);
        }

        // Console HUD: entity position widget (toggle_pos). Camera/entity debug now lives in
        // the F3 performance overlay.
        d.services.gameConsole->buildHud(playerEntry ? &playerEntry->position : nullptr);

        // Overlay layers: screen content + server notice + radio subtitles + console.
        d.services.p.renderer->submitOverlayElements(d.services.screenMgr->active().buildElements());
        d.services.p.renderer->submitOverlayElements(d.services.serverNotice.buildElements());
        d.services.p.renderer->submitOverlayElements(
            d.services.subtitleOverlay.build(d.services.subtitleQueue)); // radio/ATC callouts (#704)
        d.services.p.renderer->setConsoleElements(d.services.gameConsole->elements());

        {
            auto* h = d.session.clientHandler.get();
            d.services.perfOverlay.setPing(d.services.showPing, h && h->hasRtt(), h ? h->lastRttMs() : 0u);
        }
        updatePerfOverlay(*d.services.gameConsole, *d.services.p.renderer, d.services.perfOverlay,
                          d.services.renderBridge, *d.services.userConfig, cur == Screen::Flight,
                          d.services.cameraController.mode(), cam, playerEntry, d.services.terrainStreamer.get());

        // Automated frame capture (#909 groundwork): once the Flight session has streamed in, write one
        // PNG and quit — the reliable in-engine path for visual verification (no external screenshot tool).
        if (!d.services.screenshotPath.empty() && cur == Screen::Flight) {
            static int flightFrames = 0;
            ++flightFrames;
            if (flightFrames == d.services.screenshotFrames)
                d.services.p.renderer->captureScreenshot(d.services.screenshotPath.c_str());
            else if (flightFrames >= d.services.screenshotFrames + 2)
                running = false; // the PNG was written in endFrame; exit cleanly
        }

        d.services.p.renderer->endFrame();

        // Cinematic recorder (#916): endFrame() has just delivered this frame to the capture sink
        // (lastFrame). Push it to the encoder at every capture boundary reached, and stop when the shot
        // list / mission / wall-clock cap says so.
        if (d.services.recorder.active && cur == Screen::Flight)
            recorderEmit(running);

        d.services.p.input->flush();
        d.services.p.joystick->flush();
    }

    recorderFinish(); // close the encoder + set the exit code (#916)
}

} // namespace fl
