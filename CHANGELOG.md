# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **network**: `admin_auth_status` command shows per-IP admin auth lockout state and pending failure counts; `status` command appends a lockout count line when one or more IPs are currently locked out; closes #331
- **network**: per-IP brute-force protection for the `MsgAdminCommand` operator channel — after `admin_auth_max_failures` consecutive wrong passwords (default 5) the peer is kicked and reconnections from that IP are refused for `admin_auth_lockout_s` seconds (default 300); thresholds configurable in `[security]`; `fl::AuthTracker` promoted to `engine/net/AuthTracker.h` and reused by both `WorldBroadcaster` and `RconServer`; closes #315
- **test**: `test_flight_input_collector` — unit tests for `FlightInputCollector::poll()` covering keyboard (all 11 bindings), gamepad (buttons + axes + deadzone), HOTAS override path, combined keyboard+gamepad OR, console-open suppression, rate-limiter clock injection, and `seqNum`/`tickIndex` correctness; closes #311
- **game**: wire afterburner keyboard (`Tab`) and configurable gamepad left-shoulder button (`[controls].afterburner_button`, default index 4) in `FlightInputCollector`; sets `MsgClientInput::buttons` bit 1 so content-pack aircraft with `ab_thrust` tables can command afterburner; closes #307
- **network**: `MsgMotd` (0x08) extended with a `displaySeconds uint16_t` field at wire offset 1; fl-server exposes `[server].motd_display_s` in `server.toml` (0 = use client's `[client].motd_display_s`); `kProtocolVersion` bumped 1 → 2; closes #302
- **game**: multiplayer client connection — pass `--connect <host[:port]>` to join a remote `fl-server` without spawning a local one; `--operator-password <pw>` (or `FL_OPERATOR_PASSWORD` env var or `[client].operator_password` in user.toml) enables the admin console channel in multiplayer; main menu shows "Join Server" when connecting remotely; loading screen shows "Connecting to remote server…" and times out after 10 s; closes #240
- **renderer**: MOTD banner fade-out — alpha linearly decreases from 1.0 to 0.0 over the final 2 seconds of the auto-dismiss window; shutdown countdown notices (persistent, `visibleSeconds = 0`) are unaffected; closes #301
- **renderer**: MOTD banner display duration is now user-configurable via `motd_display_s` in `[client]` `user.toml` (default 15 s; `0` = persistent, no auto-dismiss); closes #300
- **server**: RCON per-IP failed-auth lockout — after `max_auth_failures` consecutive wrong passwords the source IP is locked out for `lockout_seconds`; locked-out connections receive an immediate `AUTH_RESPONSE id=-1` and are closed before processing any packets; both thresholds are configurable in `[rcon]` (defaults: 5 attempts, 60 s lockout); closes #297
- **test**: `test_admin_console` WorldBroadcaster integration fixture for `status` command — covers null entityManager, zero peers, and one peer using `WbFixture`; extends `WbFixture` to also wire `ctx.entityManager`; closes #318
- **test**: `test_admin_console` WorldBroadcaster integration fixture — covers all synchronous dispatch branches of the `peers` command (null broadcaster, null gameLoop, zero peers, one peer) using a real `WorldBroadcaster` object; closes #298
- **game**: main menu, loading screen, settings screen, and game-flow skeleton; game now opens to a main menu; "Sandbox (Instant Action)" starts a local server in the background while a Quake-style loading screen shows progress; in-flight Escape opens a pause menu with Resume / Settings / Quit to Menu / Exit to Desktop; `ScreenManager` drives screen transitions with automatic mouse-capture and server-pause side effects; `SettingsScreen` exposes resolution, display mode, vsync, anti-aliasing, draw distance, and master/music/SFX volumes; closes #149
- **server**: `pause` and `resume` admin commands suspend and resume the simulation tick rate via `GameLoop::setRate(TimeRate::Paused/Normal)` without dropping network connections
- **game**: gamepad primary-fire button is now configurable via `fire_button` in `[controls]` (`config/user.toml`); defaults to Right Shoulder (RB / R1, index 5); triggers the gun-burst haptic effect (#287)
- **engine**: expose `abEngaged` (afterburner lit) and `engineFailFlags` (`kEngineFail*` bitmask) through the full pipeline — `FlightState` → `MsgEntityEntry` (wire offsets 68–69, additive fields) → `EntityRenderEntry` → `HapticController`; replaces the `throttle == 100` afterburner proxy and `damageLevel >= 2` engine-failure proxy with accurate flags; `kEngineFailLeft`/`kEngineFailRight` bits deliver asymmetric left/right motor haptics when set; `kEngineCompStall` auto-triggers the compressor stall sequence; closes #286

- **network**: `admin_unlock <IP>` admin console command clears the per-IP failed-auth lockout immediately without waiting for the TTL to expire; available on stdin console and RCON channel; prints a warning (not an error) when the IP was not locked; closes #330

### Fixed

- **game**: loading screen no longer hangs indefinitely when the local server fails to start; transitions to `Phase::Failed` after 10 seconds with "Local server failed to start." and returns to the main menu; `setClockOverride` added to `LoadingScreen` so both startup and connect timeouts are fully testable without real-time waits; closes #323

### Changed

- **game**: `FlightInputCollector::poll()` now reads keyboard state through `IInput::isKeyDown` instead of `SDL_GetKeyboardState`; exposes `setClockOverride` for deterministic test injection; closes #311
- **engine**: rename debug console subsystem — `DebugConsole`→`GameConsole`, `DebugCommandRegistry`→`CommandRegistry`, `DebugCommandContext`→`CommandContext`; files moved from `engine/debug/` to `engine/console/`; `AdminConsole.h/.cpp`→`ServerCommands.h/.cpp`; CMake target `engine-debug`→`engine-console` (#292)
- **renderer**: `ServerNotice` MOTD banners now auto-dismiss after 15 seconds; shutdown countdown notices remain persistent (`visibleSeconds = 0`)
- **game**: introduce `Game` class (pimpl) encapsulating the full application lifecycle; `main.cpp` reduced from 590 lines to 20; init sequence split into six named private methods (`initPlatform`, `initWindowAndRenderer`, `initContent`, `initGameSystems`, `initNetwork`, `initGameConsole`); all state moved from `main()` locals into `GameImpl`; extract `FlightInputCollector`, `PrecipitationController`, and `CameraInput::pollModeKeys`; add named free functions for audio listener, roll angle, perf overlay, and player lookup — pure structural refactor, no behavioral change

### Added

- **network**: RCON clients now receive async command confirmations (e.g. `[admin] kicked peer N`) as a second `SERVERDATA_RESPONSE_VALUE` packet; `CommandShell` exposes `mark()` + `drainSince()` for high-water-mark polling (#304)
- **engine**: `CommandShell` base class with thread-safe output ring; `GameConsole` inherits from it; fl-server admin shell wires sim-callback output through `CommandShell` for future RCON buffering (#292)
- **engine**: `GameConsole::outputLines()` (via `CommandShell`) returns output ring copy oldest-first for testing (#292)

### Added

- **server**: Source Engine TCP RCON server (`[rcon]` section in `server.toml`); reuses all existing admin console commands; disabled by default; supports `enabled`, `port`, `password`; async commands (kick, ban, tp, etc.) return a synchronous acknowledgment string for RCON clients; responses exceeding 4086 bytes are split across multiple packets per the Source Engine RCON protocol; closes #232
- **network**: `MsgMotd` (0x08, variable-length, up to 65535 chars) delivers `[server].motd` to each connecting client after `MsgConnectAck`; multi-line text split on `\n` prints each line to the game console prefixed `[server]`; first line also shown in the server notice banner; `reload_config` hot-reloads the MOTD for subsequent connections
- **engine**: `LogLevel::Trace` added to `ILogger`; `FileLogger`, `StdoutLogger`, and `UserConfig` support the new level; enabled via `log_level = "trace"` in `user.toml`; `SceneRenderer` and `ClientNetEventHandler` emit trace-level pipeline diagnostics

### Fixed

- **network**: connecting peers now spawn at terrain height plus 500 m AGL instead of a hardcoded 2000 m; `WorldBroadcaster::onConnect` reads the atomic `m_groundElevation` (seeded from `terrainStreamer.heightAt(0, 0)` at startup) on the sim thread without a data race (#252)
- **game**: Cockpit camera (F1) now rolls the horizon correctly when the aircraft banks; the `up` vector in `CameraController` was hardcoded to world +Y, so aileron roll had no visual effect — it now tracks the entity's body +Y direction in world space
- **flight**: Elevator input no longer causes the entity to spin out of control; `iyy_kg_m2` increased from 800 to 8 000, `cm_de` reduced from −3 to −0.5, and `cm_q` reduced from −25 to −8 in `BuiltinFlightModel` — the previous values violated the semi-implicit Euler stability criterion and caused pitch-rate divergence within a single tick at any flight speed
- **flight**: Entities no longer fall through terrain; `FlightIntegrator::step` accepts a `groundElev` floor and applies a bounce response (CoR 0.35) or stops at low impact speed; `WorldBroadcaster` tracks the entity's world-XZ atomically so the fl-server admin loop can call `terrainStreamer.heightAt` at the entity's actual position and update `setGroundElevation` each 50 ms tick — the terrain streamer also follows the entity so LOD-0 chunks stay loaded at the flight location; AGL no longer goes negative because the physics floor now matches the terrain under the entity
- **game**: Camera world position (`CAM x y z`) is now always displayed in the top-right corner in all camera modes; `toggle_pos` adds a second `ENT x y z` line below it for the player entity position
- **game**: Free camera `R` reset now snaps the pivot to the player entity position instead of the hardcoded world origin; falls back to world origin when no entity is present
- **game**: `MsgClientInput` is now rate-limited to 60 Hz on the client; previously sent every render frame, triggering the server's per-peer flood guard (~60 packets/s limit) at frame rates above 60 fps and disconnecting the client
- **flight**: `BuiltinFlightModel::fuel_kg` corrected from 9,999,999 kg to 200 kg; the excessive fuel mass raised total entity mass to ~10 M kg, making T/W ≈ 0.0003 — the entity could not fly and entered 90° AoA freefall indefinitely
- **flight**: Angular rate (`omega`) and body-frame velocity clamped in `FlightIntegrator::step` (±50 rad/s and ±1030 m/s) to prevent IEEE 754 overflow to NaN during extreme flight conditions
- **game**: `LocalServer` now forwards all fl-server stdout to the game process's stderr after startup via a background thread; previously all fl-server output after "listening on" was silently discarded
- **game**: Cockpit camera now looks along body +X (the entity forward convention used by the flight model and builtin tetrahedron); cockpit view previously looked along body -Z, making the scene appear static because the entity moves in +X
- **game**: Chase camera initialises behind the entity by positioning in the -X direction (yaw = entity_yaw − 90°); previously the camera was placed at yaw+180° which orbited 90° to the side of a +X-forward entity
- **game**: Initial throttle `m_sbThrottle` is now `0.4` to match the server's pre-spooled `throttle_actual`; previously sent 0 on every frame for the first ~0.1 s, stalling the engine before the player touched the controls
- **renderer**: Vulkan overlay render guard now checks for overlay elements and console elements in addition to debug text lines; previously skipped the overlay pass when only HUD or console elements were submitted
- **game**: Free camera default orbit radius reduced from 200 m to 30 m, and default pitch changed from −10° to +30° (above the entity looking down); at 200 m the builtin tetrahedron (~10 m) subtended only ~2.7° — effectively invisible; at 30 m with pitch +30° the entity fills ~43° of FoV and its noon-lit face is directly visible

### Added

- **game**: Wire haptic feedback for flight-sim events via `HapticController`; covers gun burst, hit taken, stall buffet, afterburner ignition/sustain, engine failure, G-LOC onset, transonic buffet, GPWS double-pulse, and gear touchdown; stub entry points provided for missile launch, missile warning, compressor stall, carrier trap, hydraulic failure, and ordnance release (#127)
- **renderer**: Windshield precipitation HUD overlay; 48 semi-transparent `Line` elements animate on the cockpit windshield during Rain and Storm weather presets — blue-white streaks for rain, short white smears for snow (altitude-dependent, matching the 3D particle system), with wind-speed-driven fall rate and `windX`-driven lateral tilt (closes #211)
- **audio**: Tracks in `shuffle = true` playlist states are now randomised on state entry using Fisher-Yates; the shuffled order is preserved for the full cycle and re-shuffled on each loop (closes #168)
- **renderer**: `IHud` pure-virtual interface for aircraft head-up display; `FlightHud` is the builtin default; content packs will provide per-aircraft implementations in a future phase
- **game**: `ServerNotice` class displays server shutdown/status banners in any camera mode; set by `ClientNetEventHandler` on `MsgServerNotice`

### Fixed

- **audio**: Listener velocity is now wired from the player entity to OpenAL; Doppler pitch shift is audible when flying past positional audio sources at high speed (closes #167)
- **renderer**: Particle emitters with `spawnRate < 60/s` now produce correct output at 60 fps; fractional remainder is carried across frames instead of being truncated to zero each frame (closes #263)
- **game**: Debug console, cockpit HUD, windshield overlay, and server notices are now independent overlay layers; dangling span UB (root cause of invisible console, broken F1/F2/F4 switching, and missing tetrahedron) is eliminated
- **renderer**: `vkGetQueryPoolResults` no longer reads uninitialized timestamp query slots on the first `MAX_FRAMES_IN_FLIGHT` frames; fixes `[VK ERROR] query not reset` on startup

### Changed

- **build**: `find_package` version floors for all system-preferred deps tightened to full three-component versions matching their pinned FetchContent tags; dependency version table in `docs/development.md` updated and completed (closes #280)
- **build**: SDL3 upgraded from 3.2.10 to 3.4.10
- **build**: Lua upgraded from 5.4.7 to 5.5.0; content-pack AI scripts must not use `global` as a variable name (reserved keyword in Lua 5.5)
- **ci**: Removed `liblua5.4-dev` / `brew install lua` from all CI workflows; Lua is now built from source via FetchContent on all platforms (`liblua5.5-dev` not yet in package managers)
- **renderer**: `rain` and `storm_rain` preset spawn rates reduced from 600/s and 1200/s to 100/s and 200/s respectively; the previous values compensated for the truncation bug and also exceeded `kMaxParticles=8192` at 9 emitters

## [0.2.2] - 2026-05-27

### Changed

- Fix artifact glob and add attestations permission

## [0.2.1] - 2026-05-27

### Added

- Add fallback release notes when cliff output is empty

## [0.2.0] - 2026-05-26

### Added

- Add CMake skeleton with subdirs and dependency management (#68)
- **platform**: Define HAL interface headers (closes #14) (#76)
- **content**: Implement content pack and mod system (#78)
- **renderer**: Implement Vulkan + MoltenVK renderer backend (#79)
- **input**: Add SDL3 input backend, engine binding/axis layer, and input_test tool (#84)
- **audio**: Implement OpenAL Soft backend (closes #18) (#85)
- **network**: Implement ENet backend, fl-server binary, and network tests (#93)
- **engine**: Add i18n infrastructure (closes #20) (#95)
- **engine**: Add first-run detection and user config persistence (closes #22) (#99)
- **engine,game**: Add crash reporting, FileLogger, and fighters-legacy stub (#101)
- **engine**: Add graphics and audio mix settings to user config (#105)
- **engine,platform,game**: Boot without content pack; sandbox inspector on first run (#113)

### Changed

- Add prior-art simulator landscape and FDM RFC reference (#104)
- Remove fa-content from roadmap; pivot to fl-base-pack (#111)
- Add technology reference index and fix reuse lint (#112)
- **roadmap**: Update phase 1 acceptance criteria (#114)
## [0.0.1] - 2026-05-22

### Changed

- Add notes about fa-content repo (#1)
- Slim README to project card; extract roadmap to docs and GitHub issues (#59)

### Fixed

- Wrap SPDX example snippets with REUSE-IgnoreStart markers (#11)
