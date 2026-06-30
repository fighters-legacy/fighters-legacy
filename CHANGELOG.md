# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **ci**: 128-client scale & bandwidth perf/soak gate (`.github/workflows/scale-gate.yml`, #520). A two-tier gate built on `bot_swarm`'s `--assert-*` hooks and the authoritative `server_tick` block: a Linux `pr-gate` job (Release `fl-server`+`bot_swarm`, Vulkan-disabled) runs on every PR and hard-fails on the machine-independent metrics — downstream ≤150 KB/s/client, full admission (no refused/dropped), and a committed `downstream_kbs_per_client` baseline regression — while the CPU-timing tick-ms p99 stays advisory (the client-side tick-Hz proxy is noisy on shared runners). A nightly/`workflow_dispatch` `reference-gate` job runs the 128-client `reference`/`soak` profiles and enforces the strict ≤16.6 ms p99 tick gate only with `--strict` (meaningful on the 8-core/16 GB reference or a self-hosted runner). New `tools/bot_swarm/scale_gate.py` driver (cross-platform; reads `scale-gate.json` profiles, runs `run_loadtest.sh`/`.ps1` once per pattern with the profile's asserts, evaluates each report, diffs the KB/s baseline, writes a `$GITHUB_STEP_SUMMARY` table), a `run_loadtest.ps1` Windows-parity launcher (smoke-run on every PR so it can't bitrot), and `run_loadtest.sh` extended with a `-- <extra bot_swarm args>` pass-through, `FL_LOADTEST_REPORT`/`FL_LOADTEST_PORT` overrides (distinct port per pattern avoids the UDP rebind race), and a soak RSS leak sampler. Committed KB/s baseline in `scale-gate-baseline.json`; pure-logic coverage in `tests/test_scale_gate.py`. See `docs/load-testing.md` (#520)

- **network**: adaptive per-client send-rate and congestion response — each connected peer owns an AIMD `engine/net/CongestionController` that the server steps every tick from that peer's ENet link quality (new `INetwork::getPeerLinkStats` → packet loss, RTT, reliable bytes in flight). On a degraded link (loss above threshold, RTT a margin above its running baseline, or a large reliable backlog) the controller lowers a per-peer throttle that drives two levers: it **decimates the snapshot send rate** (60 Hz down toward a configurable floor, default 10 Hz) and **scales the per-client byte budget down** (the #516 scheduler then defers more low-relevance entities). A healthy peer, the zero-link-stats case (mocks/loopback), or a disabled controller all stay at the full 60 Hz / full budget, so there is no behaviour change on a clean link and no wire-format change. The delay signal uses ENet RTT (not the snapshot one-way delay, which our own decimation would inflate — a self-reinforcing collapse). Config: `[world] congestion_enabled` / `congestion_min_send_hz` / `congestion_loss_threshold` / `congestion_budget_floor_bytes` (all hot-reloadable via `reload_config`); per-peer send rate and loss are shown by the `peers` admin command. Covered by `test_congestion_controller` + new `[congestion]` cases in `test_world_broadcaster`, plus `test_server_config`/`test_network`/`test_admin_console`. See `docs/congestion-control-design.md` (#518)

- **docs**: `docs/project-management.md` — consolidates the project's evolved work-management methodology (issue types Epic/Feature/Task/Spike/Bug, epics with native sub-issues, the `component:`/RFC label families, milestones-as-phases, and the single Project board with its Roadmap/Board/Open Items views and fields) into one portable reference, with a "Lessons & Rev 2" retrospective and a copy-this checklist for new projects. Adds `Epic` and `Spike` issue-form templates under `.github/ISSUE_TEMPLATE/` and links the doc from the README and docs index
- **network**: per-client priority/budget snapshot scheduler — each peer's snapshot is capped at a configurable byte budget (`[world] snapshot_budget_bytes`, default 1200, 0 = unlimited; hot-reloadable via `reload_config`). The new pure `engine/net/SnapshotScheduler` ranks the visible entities by relevance (distance, closing-speed, recency, player-owned) and sends only the highest-priority set that fits; a recency term guarantees no entity is starved, and the peer's own entity is always included. Bounds per-client bandwidth as the population grows. The server emits an explicit `SnapshotDespawn` TLV (`ExtTag::SnapshotDespawn = 0x0103`) for entities removed from the sim, and `ClientNetEventHandler` now retains entity state across snapshots (timeout eviction at `kSnapshotRetentionTicks`) so budget-deferred entities don't flicker. Covered by `test_snapshot_scheduler` + new cases in `test_snapshot_codec`/`test_wire_codec`/`test_world_broadcaster`/`test_client_net_event_handler` (#516)
- **network**: 3D (XYZ) interest management — the per-peer snapshot interest query now applies an exact full-3D squared-distance gate against the receiving peer's position, so entities far in altitude (in the same XZ spatial-hash cell but beyond the interest sphere) are correctly culled rather than included. Replaces the previous conservative XZ-cells-only selection (closes #402)
- **engine**: snapshot quantization codec — new header-only `engine/net/BitStream.h` (MSB-first `BitWriter`/`BitReader`) + `engine/net/Quantization.h` (smallest-three quaternion encode, offset-binary fixed-point) + `engine/net/SnapshotCodec.{h,cpp}` (the shared `QuantEntity` encode/decode used by both server and client, with tunable bit-budget constants). Endianness-independent and byte-deterministic across MSVC/GCC/Clang on x86-64 + ARM64; covered by `test_snapshot_codec` (#515)
- **engine**: data-parallel sim tick — new `engine-job` library (`JobSystem`: persistent worker pool + blocking `parallel_for` with dynamic chunk claiming). `WorldBroadcaster::onTick` now runs the per-entity work as two parallel passes (AI `sample()` over a consistent pre-step snapshot, then `FlightIntegrator` integrate with disjoint per-entity writes) dispatched through an injected `JobSystem`, or inline when none is set. Serial-equivalent by construction (per-entity deterministic turbulence RNG, no cross-entity writes, no parallel float reduction) — bit-identical across worker counts, validated race-free under a new ThreadSanitizer preset/CI (`tsan`). Sized by `[world] sim_worker_threads` (0 = auto, 1 = serial) / `fl-server --sim-worker-threads`; single-player runs serial. Resolves the Epic A design spike (#510) and lands the parallel AI/integrate passes (#511); see `docs/server-job-system-design.md`
- **engine**: server tick-budget instrumentation — `engine/perf/TickProfiler` records per-phase wall-time (maintenance/integrate/ai/collision/serialize/total) every `WorldBroadcaster::onTick`, exposed via `WorldBroadcaster::getTickBudget()`. `fl-server` surfaces it through the `status` command (now reports real tick Hz + mean/p99 ms), a new `tickstats` admin command, and a `[metrics] tick_json_path` / `--metrics-json` atomic JSON export (`engine/perf/ServerTickReport`). `bot_swarm --server-metrics` embeds the authoritative `server_tick` block (schema_version 2) alongside the client-side proxy, with an `--assert-max-tick-ms` p99 gate hook for #520 (#513)
- **network**: adaptive per-peer jitter buffer resizing — EWMA of one-way delay + RFC 3550 inter-arrival jitter estimator continuously drive `JitterBuffer::setMaxDepth()` via `[world].jitter_buffer_adapt_window`, `jitter_buffer_hysteresis`, and `jitter_buffer_jitter_multiplier`; hot-reloadable via `reload_config` (#424, #429)
- **network**: `WorldBroadcaster::forEachPeer` callback now receives a `PeerInfo` struct, adding `bufferMaxDepth`, `ewmaDelayTicks`, and `ewmaJitterTicks` fields alongside existing fields
- **network**: `peers` admin command output extended to show EWMA delay, jitter estimate, and buffer max depth
- **ai**: `patrol_attack` and `escort` StateMachineController templates in AiControllerFactory (#430)
- **ai**: LagPursuitController for guns employment on turning targets; completes the pursuit triangle alongside pure pursuit and `LeadPursuitController` (#432)
- **engine**: `engine-world` foundation library — `AlertLevel`/`EscalationStage` enums, `FactionDef`/`FactionRegistry` (O(1)-by-index faction store with symmetric relationship graph and mutex-guarded per-faction alert levels), and the `AirspaceZone` descriptor; plus an `EntityState::factionIndex` seam. Foundation types only — `AlertSystem` (#162) and `IWorldAiProvider` (#163) build on top (#415)
- **tools**: `bot_swarm` headless multi-client load generator — connects N synthetic clients to a running `fl-server`, sustains `MsgClientInput` via a pluggable `IFlightPattern` (`weave`/`level`/`aggressive`/`idle`/`random`), and reports connect success, downstream KB/s per client, RTT, and an observed-server-tick-Hz proxy (from snapshot `tickIndex` progression). Includes a `run_loadtest.sh` runner and `docs/load-testing.md`; shares `tools/common/NetStats.h` with `net_check`. First step of the 128+ scale gate (Epic I, #519)

### Changed

- **network**: client-acked delta baselines replace the fixed `baseline_interval_ticks` re-sync. The server now drives full-vs-delta off what each client has actually acknowledged (the last-received snapshot tick the client already echoes in `MsgClientInput`/`MsgHeartbeat`): an entity is re-sent as a full record every tick until the peer acks the tick its full streak started on, then converges to deltas. This removes the globally-synchronized periodic full-resync spike across all peers and recovers a dropped full in ~1 RTT instead of up to 2 s, with no wire-format change. The client now ignores out-of-order/duplicate snapshots so its echoed tick stays a monotonic ack. The `[world] baseline_interval_ticks` config field, its `reload_config` handling, and `WorldBroadcaster::setBaselineInterval` are removed (a stale key in `server.toml` is silently ignored); the per-entity `kSnapshotRetentionTicks` force-full (interest-out re-entry) is retained. Covered by new cases in `test_world_broadcaster`/`test_client_net_event_handler` (#517)
- **network**: **breaking wire change** — the per-tick `MsgWorldSnapshot` entity body is now a quantized, bit-packed record stream instead of fixed 88-byte `MsgEntityEntry` + 64-byte `MsgEntityUpdate` records (both removed). `MsgWorldSnapshotHeader` grows 16→40 bytes, adding a `double frameOrigin[3]` (per-snapshot position-quantization origin = the receiving peer's position), a `recordCount`, and a `bitstreamBytes` length; the TLV extension block now starts at `40 + bitstreamBytes`. Each record carries a `full` bit (full records add typeIndex + gen; deltas omit them), position is fixed-point relative to the frame origin (planet-scale accurate without a `double` per record), orientation uses smallest-three, and velocity/omega/byte-fields are quantized; omega is sent only for the receiving peer's own entity. `kProtocolVersion` stays 1 (primary development). See `docs/snapshot-quantization.md` and `docs/network-protocol.md` (#515)
- **server**: Raised the config validation ceilings — `max_peers` 128→1024, `connect_rate_limit_count` 100→100000, `max_connections_per_ip` 128→1024 — so the load harness can drive the server past 128 to characterise the transport ceiling. A testing affordance, not a capacity guarantee (#519)
- **network**: `ENetNetwork` now ref-counts `enet_initialize`/`enet_deinitialize` (mutex-guarded) so many client hosts can coexist in one process with staggered/concurrent lifetimes — one instance's `shutdown()` no longer tears ENet down for the others (#519)

- **docs**: Re-target the roadmap to 128+ simultaneous players — multiplayer is now a co-equal product pillar (PvP + co-op PvE + persistent world). Revises the locked `32+` decision via a dated decision record (`docs/architecture.md`), inserts a new **Phase 5 — Multiplayer at Scale & Live Services** (renumbering former Phases 5–8 to 6–9), adds scaling seams to Phases 3–4 (transport replacement behind `INetwork`, server job system, wire quantization, load harness), and documents the self-host-only hosting model plus the planned Go companion repos (`fl-account`, `fl-review`, `fl-operator`). Docs only; no code or wire changes in this entry

### Fixed

- **game**: Split multi-line admin responses into one game console entry per line (#417)

## [0.2.5] - 2026-06-26

### Added

- **game**: Load bindings.toml and wire per-axis AxisConfigTable (#257) (#393)
- **game**: Migrate fireButton/afterburnerButton to bindings.toml (#394)
- **network**: Extensible TLV extension block framing (#347) (#395)
- **ai**: Server-side AI flight controller framework (#352) (#398)
- **network**: Connection heartbeat, idle timeout, and ping overlay (#362) (#399)
- **flight**: Double-precision FlightState velocity vector for ICBM-range trajectories (#387) (#400)
- **engine**: Spatial partition / broadphase index for neighbor and range queries (#360) (#401)
- **network**: Snapshot interest management and delta compression (#346) (#404)
- **network**: Drain async sim-thread shell output on ENet admin channel (#377) (#407)
- **engine**: Add WeatherPreset::Snow and WeatherPreset::Blizzard (#269) (#411)
- **ai**: Wire LuaSandbox to IEntityController for Lua-scripted AI (#359) (#414)
- **network,game**: Per-peer latency TLV in MsgWorldSnapshot and cockpit HUD indicator (#382) (#420)
- **network**: Per-peer jitter buffer for MsgClientInput delivery (#423)
- **network**: Client-side prediction and state reconciliation (#428)
- **ai**: StateMachineController with Condition-gated transitions (#397) (#431)
- **ai**: Lead pursuit and ACM maneuver controllers (#396) (#433)
- **network**: Injectable clock for RconServer drain timing (#435)
- **renderer**: Atmospheric sky, GTAO, aerial perspective, biome terrain (#437) (#442)

### Changed

- Point policy contacts at the fighterslegacy.org domain (#409)
- **engine**: Migrate all native codebase types into namespace fl (#419)
- Align renderer docs with #437 render-graph changes and add rendering.md (#455)

### Fixed

- **network**: Admin_auth_status detail over network channels; wall-clock ENet drain deadline (#418)
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
