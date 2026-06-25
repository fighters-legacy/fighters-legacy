# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **ai**: five advanced ACM controllers in `engine-ai`: `LeadPursuitController`
  (proportional navigation — steers toward a predicted intercept point rather than the
  target's current position; `navGain` controls lead scaling: 0.0=pure pursuit,
  1.0=first-order TTC lead), `ImmelmannController` (half-loop + roll to reverse heading
  with altitude gain), `SplitSController` (roll inverted + pull-through for altitude-trading
  reversal; deploys `ControlInput::speedbrake` on roll-in to limit entry airspeed),
  `HighYoYoController` (climb-away energy-management overshoot correction; banks away from
  target in Climb phase, back toward target in Reacquire phase), `LowYoYoController`
  (dive-and-cut-corner to close on a turning target; unloads in Dive phase then pulls up in
  Pull phase). All five registered in `AiControllerFactory` (`lead`, `immelmann`, `split_s`,
  `high_yo_yo`, `low_yo_yo`) and composable via `StateMachineController` (closes #396).

- **ai**: `StateMachineController` in `engine-ai` sequences `IEntityController` child
  controllers with named states, priority-ordered `Condition`-gated transitions, and
  per-transition `minDwellSeconds` hysteresis. Each state's child is constructed fresh on
  entry (resetting mutable state such as `BreakTurnController`'s phase timer). Built-in
  condition helpers: `ThreatWithinRange`, `ThreatBeyondRange`, `HpBelow`,
  `AnyEntityWithinRange`, `Always`, `And`, `Or`, `Not`. Enables patrol-attack-retreat and
  other multi-phase AI behaviors without custom `IEntityController` subclasses per
  scenario. `AiControllerFactory` now includes `StateMachineController.h` so all condition
  helpers are available to factory callers (closes #397).

- **network**: client-side prediction for the player's own entity: local `FlightIntegrator`
  applies inputs immediately before the server round-trip; each `MsgWorldSnapshot` triggers
  reconciliation — the integrator is reset to the server's authoritative state and the last
  `estimatedDelayTicks` inputs are replayed forward. Divergence above `snap_threshold_m`
  (default 5 m) hard-snaps; smaller divergence blends at `blend_rate` per reconciliation.
  Configurable via `[prediction]` in `user.toml` (`enabled`, `snap_threshold_m`,
  `blend_rate`). Other entities remain server-authoritative with velocity extrapolation
  unchanged (#381).

- **network**: `MsgEntityEntry` and `MsgEntityUpdate` now carry `omega float[3]`
  (body-frame angular rates p, q, r in rad/s) for accurate prediction reconciliation.
  Struct sizes: `MsgEntityEntry` 72 → 88 bytes, `MsgEntityUpdate` 52 → 64 bytes (#381).

- **network**: `ExtTag::SnapshotPeerDelayTicks` (0x0102) — raw `estimatedDelayTicks`
  as a `uint16_t` TLV in each per-peer `MsgWorldSnapshot`, companion to the existing
  `SnapshotPeerLatency` (ms). Avoids ms-rounding loss for the prediction replay depth
  (#381).

- **server**: injectable `IClock` seam for `RconServer` — `setClock(const IClock&)` controls
  drain-deadline timing and auth-lockout expiry for deterministic unit tests; propagates to the
  internal `AuthTracker`; defaults to `SystemClock::instance()` with no behavior change
  (closes #416).

- **network**: server-side per-peer jitter buffer for `MsgClientInput` delivery; initial depth
  seeded from `estimatedDelayTicks` (clamped to `[world].jitter_buffer_depth`); drains one input
  per sim tick; stale-repeats the last drained input when the buffer is empty to prevent coasting
  under packet loss; configurable via `[world].jitter_buffer_depth` (default 4, hot-reloadable via
  `reload_config`). `peers` admin command now shows per-peer input queue depth (`q=N`) (#380).

- **network,tools**: `tools/latency_analysis/` — per-platform ENet loopback latency
  benchmark scripts (`measure_linux.sh`, `measure_macos.sh`, `measure_windows.ps1`) and
  result comparator (`compare.py`); `getPeerRtt()` added to `INetwork`/`ENetNetwork`;
  `net_check --bench` mode for 60 Hz RTT sample collection; decision record and re-run
  runbook in `docs/loopback-latency-analysis.md` (#179).

- **network,game**: `ExtTag::SnapshotPeerLatency` (0x0101) TLV appended to each peer's
  `MsgWorldSnapshot`; client stores and displays it as a compact `"42 ms"` indicator in `FlightHud`
  (cockpit mode only). Configurable via `[hud].show_latency` in `user.toml` (default `true`);
  hidden automatically when `estimatedDelayTicks == 0` (single-player localhost) (#382).

- **engine**: `LuaController` — `IEntityController` backed by a sandboxed Lua 5.5 script;
  exposes entity state, `guidance.*` math, `nearby_entities()`, and `get_entity()` to scripts;
  per-entity Lua state persists between ticks for stateful behaviors (#359).
- **engine**: `EntityDef.aiScriptId` — content packs can specify a default Lua AI script per
  entity type via `ai_script` in entity TOML; auto-assigned when spawning without `--ai` (#359).
- **server**: `spawn --ai lua <name>` admin command — spawn entities with a Lua AI controller
  loaded from the content pack's `ai/` directory; works from stdin and ENet admin channels (#359).
- **engine**: `AssetManager::findPackRootForAsset()` — locates the owning content pack's root
  directory for a given asset, used to configure `require()` in `LuaController` (#359).
- **engine**: `AssetManager::listAssets(AssetType)` — generic asset listing across all packs
  (replaces the mission-specific internal implementation of `listMissions()`).
- **docs**: `docs/modding/ai.md` — complete Lua AI scripting API reference (Phase 3 acceptance
  criterion); documents `compute_control`, `guidance.*`, `nearby_entities`, `get_entity`, and
  includes worked loiter and proximity-pursuit examples (#359, partially addresses #185).

- **engine,game**: Add `WeatherPreset::Snow` (5) and `WeatherPreset::Blizzard` (6); snow is now
  server-authoritative and altitude-independent (#269). Replaces the client-side altitude proxy
  from #265 with explicit precipitation type propagated via `MsgWeatherState.preset`. Adds
  `set_weather snow` and `set_weather blizzard` to the fl-server admin console and the in-game
  `set_weather` command. Snow/Blizzard are excluded from the autonomous weather cycle and
  persist until an operator changes the preset.

### Changed

- **engine**: Migrated all native codebase types into `namespace fl`; `fl::` is now the single
  project namespace throughout `platform/`, `engine/`, `game/`, `server/`, and `tools/`. Closes
  #392. The only documented exception is `engine/audio/ogg_impl.h` (C-linkage stb_vorbis
  wrapper — cannot use C++ namespaces).

- **tools**: `scripts/roadmap-status.sh` now reports progress from the repository's GitHub
  issue milestones (closed/total issue counts + due date per `Phase N` milestone) instead of
  the GitHub Project's "Phase" single-select field, which was removed. Pulls milestones via a
  single REST call; derives each phase's elapsed window sequentially from milestone due dates
  (milestones have no start date) to keep the on-track/at-risk/behind/overdue signal.

- **docs**: completed the `jomkz` → `fighters-legacy` GitHub org migration for in-repo
  references — rewrote repository URLs (README badges, roadmap milestones, `SUPPORT.md`,
  issue-template config, the release scripts, and the compiled issue-report URLs in
  `game/fighters-legacy/Game.cpp`) to `fighters-legacy/fighters-legacy`; moved the user-data
  vendor namespace from `jomkz` to `mkzsystems` (`SDL_GetPrefPath` plus the
  `docs/development.md` path table); removed the repo-local `SECURITY.md`,
  `CODE_OF_CONDUCT.md`, and `TRADEMARK.md` in favour of the org-wide `fighters-legacy/.github`
  defaults, repointing inbound links to their canonical `.github` URLs; and pointed
  `CODEOWNERS` at the `@fighters-legacy/maintainers` team. Also repointed the content-pack
  repository references (`fa-content` in `CLAUDE.md`, `fl-base-pack` in
  `docs/architecture.md`) to the org now that those repos have been transferred.

- **docs**: clarified the distribution & monetization strategy in `docs/distribution.md`
  — adopts a softened, Ardour-style "sell convenience, not the game" model (GPL §6 makes
  binaries non-gateable; first-party builds are paid and framed as supporting
  development, Flathub and community repackages stay free), and documents trademark +
  proprietary content as the real revenue levers. Reassigned codebase copyright holder
  from "John McKenzie" to **MKZ Systems LLC** across `REUSE.toml` and inline tool/script
  SPDX headers.

- **flight**: `FlightState::vel_body[3]` upgraded from `float[3]` to `double[3]` for
  ICBM-range trajectory precision; adds `quatRotateD` for double-precision world-frame
  position integration; unblocks ballistic force model (#354) and missile guidance (#355)
  (#387)

### Added

- **docs**: wired concrete project contact addresses into the policy files ahead of the
  move to the `fighters-legacy` GitHub org — `security@fighterslegacy.org` in `SECURITY.md`
  and `trademark@fighterslegacy.org` in `TRADEMARK.md`.

- **docs**: `TRADEMARK.md` — a permissive trademark policy for the **Fighters Legacy™**
  mark (faithful community rebuilds may keep the name; modified builds must rebrand), an
  `AUTHORS` file recording author credit (John McKenzie) separately from the copyright
  holder (MKZ Systems LLC), and an explicit copyright + trademark notice in the README
  License section.

- **network**: ENet admin channel (`MsgAdminCommand`) now delivers deferred `CommandShell`
  output written inside `enqueueSimCallback` lambdas as follow-on `MsgAdminResponseChunk`
  packets on the next sim tick, matching the async drain behaviour already present in the RCON
  channel. Commands like `spawn`, `kill`, `tp`, `ban`, and `peers` previously sent only a
  brief queued-ack over ENet; the actual confirmation (e.g. `"[admin] spawned … entity=N/N"`)
  now arrives as a second response packet sharing the same `reqId`. Wired via
  `WorldBroadcaster::setAdminShell()` (std::function injection, no new CMake dependencies).
  (#377)

- **network**: Snapshot interest management + delta compression for 20+ player scale (#346):
  `WorldBroadcaster::onTick()` now sends a per-peer unicast `MsgWorldSnapshot` via
  `INetwork::send()` containing only entities within `draw_distance_km` of the peer's own
  entity (via `SpatialIndex::queryRadius()`). Entities already known to a peer appear as
  compact `MsgEntityUpdate` records (52 bytes, float positions) instead of full
  `MsgEntityEntry` records (72 bytes); a full baseline re-sync fires every
  `baseline_interval_ticks` ticks (default 120 = 2 s) for UDP packet-loss recovery.
  `MsgWeatherState` and `MsgServerNotice` remain global broadcasts. Configurable via
  `[world] draw_distance_km` (default 200 km) and `[world] baseline_interval_ticks` (default
  120) in `server.toml`; both hot-reloadable via `reload_config`. New wire struct
  `MsgEntityUpdate` (52 bytes, naturally aligned); `MsgWorldSnapshotHeader::entityCount`
  renamed to `fullEntityCount`; new `updateCount` field at @4 (repurposed from `reserved`
  uint32 — same 16-byte header size, same wire positions).
- **engine**: `engine-spatial` library — `fl::SpatialIndex`, a 2D uniform spatial hash (XZ
  bucketing, double-precision, configurable cell size, default 10 km) for entity neighbor and
  range queries at planet scale; `clear()` + `insert()` for per-tick rebuild; template
  `queryRadius(center, radius, fn)` visits all candidate entities in O(cells × local density);
  foundational for snapshot interest management (#346), drone-swarm AI (#353), and AoE warhead
  blast radius (#356) (#360)
- **engine**: `WorldBroadcaster` rebuilds `SpatialIndex` once per sim tick (at tick start) and
  exposes `spatialIndex() const noexcept` for interest management and AoE consumers (#360)
- **engine**: `IEntityController::sample()` gains optional 4th parameter
  `const SpatialIndex* si = nullptr`; all existing controllers ignore it; `WorldBroadcaster`
  passes `&m_spatialIndex` each tick so future AI controllers can query neighbors without an
  additional pass (#360)
- **network**: Connection heartbeat / keepalive (issue #362): clients now send `MsgHeartbeat`
  (0x0B, 16 bytes, 1 Hz) when flying; the server replies with `MsgPeerDelay` (0x0C, 4 bytes)
  carrying `estimatedDelayTicks` so the client can display an approximate RTT.
- **network**: `show_ping` console command toggles a "Ping: N ms" overlay line; displayed
  regardless of F3 performance-overlay mode.
- **network**: `[security] idle_timeout_s` fl-server config (default 0 = disabled): disconnect
  peers that send no `MsgClientInput` or `MsgHeartbeat` for N seconds.

- **ai**: `engine-ai` library: `LoiterController` (configurable-direction orbit),
  `WaypointController` (sequential 3D waypoints), `PursuitController` (pure-pursuit intercept),
  `EvadeController` (horizontal escape from a threat), `BreakTurnController` (two-phase
  defensive ACM: roll toward threat then maximum-G pull); `Guidance.h` header-only math
  utilities; `AiControllerFactory` for string-based controller creation from admin commands (#352)
- **server**: `spawn` admin command extended with `--ai <behavior> [args...]` to attach an AI
  controller at spawn time; supported behaviors: `loiter`, `waypoint`, `pursuit`, `evade`,
  `break` (#352)
- **network**: Extensible TLV extension block support for wire messages (`WireCodec.h`):
  `appendExt`, `appendExtRaw`, `findExt`, `readExtValue`; `ExtTag` registry in
  `GameProtocol.h`; old receivers skip unknown extensions transparently (#347)
- **network**: `MsgWorldSnapshot` carries `SnapshotPeerCount` TLV extension
  (`ExtTag::SnapshotPeerCount = 0x0100`, `uint16_t`); readable on the client via
  `ClientNetEventHandler::serverPeerCount()` (#347)

### Changed

- **game**: `FlightInputCollector` reads `FireWeapon` and `Afterburner` gamepad bindings from
  `bindings.toml` `[alt]` section (`GamepadButton` or `GamepadAxis` threshold ±0.5 f);
  `fire_button`/`afterburner_button` removed from `[controls]` in `user.toml` (#312)
- **game**: Gamepad axis mapping and per-axis deadzone/curve/invert/scale now loaded from
  `config/bindings.toml` (`[alt]` + `[axis_config]`), replacing the global `gamepad_deadzone`
  and `invert_*` fields in `user.toml [controls]`; `InputBindings` table stored in game
  services for Phase 4 key-remapping; gamepad button defaults corrected to `RightShoulder`
  (fire) / `LeftShoulder` (afterburner) to match existing `ControlsSettings` defaults (#257)
- **engine**: All `engine-input` types (`InputBindings`, `AxisConfigTable`, `AxisConfig`,
  `Binding`, `BindingSource`, `InputAction`) moved into `namespace fl` (#257)
- **engine**: `fl::ensureAndReadConfig` / `fl::writeConfigFile` now take `const
  std::filesystem::path&` instead of `const std::string&` to avoid Windows locale-encoding
  issues with UTF-8 paths from SDL (#257)

### Fixed

- **network**: `admin_auth_status` now delivers per-IP lockout and failure detail to both RCON
  and ENet admin clients as the synchronous response body; previously, only fl-server stdout
  received the detail because `printAuthSection` wrote to the shell ring before the drain mark
  was taken (#405).
- **network**: ENet admin drain now uses a 20 ms wall-clock deadline (matching the RCON drain)
  instead of `fireAfterTick = currentTick + 1`, preventing silent drain loss when `GameLoop`
  catch-up batches multiple ticks per iteration before draining `enqueueSimCallback` callbacks
  (#406).

## [0.2.4] - 2026-06-20

### Added

- **renderer**: Advanced graphics quality settings — shadow, particles, AA mode (#235) (#376)
- **network**: Stream long admin responses as MsgAdminResponseChunk (#239, #361) (#378)
- **network**: Unreliable MsgClientInput, seqNum staleness guard, delay estimation (#348) (#379)
- **network**: Configurable peer spawn points with terrain height caching (#383)
- **engine**: Remove flat-Earth mode; spherical physics is now the only mode (#384) (#385)
- **renderer**: Per-vertex spherical terrain mesh correction (#370) (#386)
- **flight**: Double-precision FlightState position vector (#371) (#388)

### Changed

- Restructure roadmap from 6 phases to 8, deferring OpenGL (#374)

### Fixed

- **renderer**: Correct sandbox load-in, camera, and glTF mesh winding (#389)
## [0.2.3] - 2026-06-15

### Added

- **engine,game**: Add fixed-timestep game loop with sim thread and time compression (#119)
- **network**: Add fl-server operator config spec and expanded config (#121)
- **platform**: Add IAsyncFilesystem interface and SDL3 worker-thread backend (#122)
- **platform**: Add IDisplay interface and SDL3 backend (#71) (#123)
- **platform**: Add ICursor interface and SDL3 backend (#72) (#124)
- **platform**: Add IJoystick interface and SDL3 backend (#69) (#125)
- **platform**: Add rumble capability queries and stopRumble to IInput (#126)
- **platform**: Add getKeyName to IInput and SDL3 backend (#75) (#129)
- **flight**: Add 6-DOF stability-derivative flight model (#132)
- **engine**: Add difficulty & accessibility settings (#44) (#133)
- **engine**: Add entity/object system (#31) (#135)
- **tools**: Asset pipeline for fl-base-pack (#109) (#136)
- **renderer**: Add modern scene renderer with HDR pipeline, PBR lighting, and GPU particles (#139)
- **tools**: Add headless Blender aircraft mesh generator (#141)
- **audio**: Ogg streaming, music playlists, subtitle queue, and voice callout infrastructure (#35) (#164)
- **network**: Server-authoritative sim loop, ENet client rendering, and perf overlay (#169)
- **engine**: Promote world positions to double precision (#170) (#177)
- **content**: Terrain format v2 — chunk_size_m, LOD levels, chunk path convention, theater manifest (#190)
- **network**: Enet6 IPv6 dual-stack, platform factories, and tool abstraction (#180) (#192)
- **content**: Add IContentPack::resolveTerrainChunk for mod-stack chunk override (#193)
- **renderer**: Procedural terrain generation and chunk I/O (#188) (#195)
- **renderer**: TerrainStreamer — async chunk lifecycle, LOD rings, and height queries (#173) (#196)
- **network**: Authoritative game protocol (#142) (#197)
- **flight**: Builtin UFO flight model and Y-up coordinate alignment (#198)
- **network**: Protocol version negotiation (#92) (#200)
- **network**: Server LAN discovery via UDP broadcast (#91) (#201)
- **renderer**: Minimal HUD + camera system (#148) (#202)
- **engine**: In-game debug console (#151) (#204)
- **renderer**: Upgrade HUD font from CP437 to GNU Unifont (#203) (#205)
- **engine**: Weather & time of day (#39) (#213)
- **network**: Fl-server stdin admin console (#89) (#214)
- **network**: Fl-server connection rate limiting and DDoS mitigation (#88) (#223)
- **network**: Fl-server delayed shutdown with countdown notifications (#90) (#224)
- **network**: Authenticated admin command channel for fl-server operators (#229) (#238)
- **content**: Content pack security hardening — manifest sanitization, Lua sandbox, and asset validation gates (#131) (#244)
- **engine**: Pilot profile and client-side session state (#150) (#251)
- **server**: Wire headless terrain stack into fl-server (#174) (#253)
- **game**: Hud altitude display above ground level (agl) (#254)
- **game**: Gamepad axis flight controls with deadzone and invert config (#217) (#258)
- **network**: Pre-handshake ENet flood mitigation via intercept callback (#221) (#259)
- **network**: Per-IP concurrent connection limit for fl-server (#222) (#260)
- **game**: HOTAS and raw joystick axis flight control mapping (#256) (#261)
- **renderer**: Directional particle emitters for precipitation (#206) (#262)
- **renderer**: Fractional particle spawn accumulation (#263) (#266)
- **renderer**: Wind-influenced precipitation direction (#264) (#268)
- **renderer**: Snow particle preset (#265) (#270)
- **network**: Add --reason flag to shutdown command (#225) (#274)
- **flight**: Use relative airspeed for wind in FlightIntegrator (#208) (#276)
- **flight**: Use relative airspeed for wing-sweep Mach scheduling (#277)
- **audio**: Shuffle track order in MusicManager playlists (#168) (#279)
- **renderer**: Add windshield rain and snow cockpit HUD overlay (#285)
- **game**: Wire haptic feedback events to IInput rumble API (#127) (#288)
- **network**: Deliver MsgMotd (0x08) to connecting clients on join (#293)
- **network**: Add Source Engine TCP RCON server to fl-server (#232) (#299)
- **renderer**: Auto-dismiss MOTD banner after 15 seconds (#291) (#303)
- **network**: Send delayed admin command confirmations to RCON clients via CommandShell drain (#304) (#306)
- **engine**: Expose flight telemetry flags for haptic accuracy (#286) (#310)
- **game**: Wire gamepad fire button to weapon-fired haptic event (#313)
- **game**: Main menu, settings screen, and game-flow skeleton (#149) (#314)
- **network**: Add per-IP failed-auth lockout to RCON server (#317)
- **renderer**: Configurable MOTD display duration via [client].motd_display_s (#320)
- **renderer**: Animate MOTD banner fade-out over final 2 s (#301) (#321)
- **game**: Add --connect flag for multiplayer client connection (#240) (#324)
- **network**: Configurable per-server MOTD display timeout via MsgMotd wire field (#325)
- **game**: Wire afterburner key binding in FlightInputCollector (#326)
- **game**: Make FlightInputCollector::poll() unit-testable via IInput and injectable clock (#327)
- **network**: Per-ip brute-force protection for MsgAdminCommand channel (#329)
- **network**: Admin_unlock <IP> command to clear per-IP auth lockout (#336)
- **network**: Admin_auth_status command and status lockout count (#331) (#337)
- **game**: Richer LocalServer startup failure reason for loading screen (#340)
- **network**: Admin_unlock also clears RCON auth lockout (#335) (#341)
- **network**: Admin_auth_status shows both admin and RCON channel lockout state (#342)
- **game**: Richer connection failure reason in LoadingScreen (#344)
- **network**: Send MsgConnectRefusal reason before disconnecting rejected peers (#343) (#345)
- **engine**: Spherical-Earth world model (#357) (#368)

### Changed

- **i18n**: Decouple Localization from IContentPack (#134) (#137)
- Redefine Phase 3 as OpenGL renderer; add HUD/menu to critical path (#161)
- Phase 2 audit — fix ENet version, IPv6 planning, roadmap scope split, and Windows contributor setup (#189)
- **network**: Stable wire protocol spec — field sizes, endianness, ARM64 alignment, and 128-player scalability (#199)
- **engine**: Extract shared utilities and restructure game/server entry points (#227) (#230)
- **game**: Decompose main.cpp into Game class and named init methods (#296)
- **engine**: Rename debug console subsystem to engine-console (#292) (#305)
- **network**: Add WireCodec and re-lay wire structs for natural alignment (#364)
- **network**: Bundle WorldBroadcaster setup into applyConfig and factor rejectConnection (#366)
- Phase 2b cleanup - typed session status, Game DI split, shared IClock, and vehicle seams (#367)

### Fixed

- **game**: Sandbox polish — sky, console input, HUD, throttle controls (#218)
- **renderer**: Correct sky viewDir NaN, ground colour, and camera start altitude (#220)
- **build**: Statically link SDL3, OpenAL Soft, and KTX (#241)
- **renderer**: Pixel-density handling for HiDPI and Retina display (#147) (#243)
- **engine**: Populate windX/windZ in WeatherController::computeEnvironment() (#272)
- **audio**: Wire listener velocity from player entity for Doppler shift (#278)
- **game**: Replace GameHud with independent overlay layers; fix Vulkan query pool (#289) (#290)
- **network**: Spawn peers at terrain height plus 500 m AGL (#252) (#294)
- **renderer**: Rotate windshield rain streaks by aircraft roll angle (#295)
- **game**: Add startup timeout to LoadingScreen Phase::StartingServer (#334)
## [0.2.0] - 2026-05-27

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
