# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **docs**: **Dynamic World & Agentic AI initiative (Epics M–P, #589/#590/#591/#592)** and the 2026-07-01 strategic re-plan. New `docs/ai-architecture.md` (provider seam, world-state/MCP surface, epic designs, security + degradation + testing model — "CI never requires a model"; platform matrix); a second cross-cutting initiative section in `docs/roadmap.md` (epics M–P with dependency order + acceptance gate, Critical Path renumbered + item 9, Phase 4/8 acceptance updated); two dated decision records in `docs/architecture.md` (agentic-AI architecture — pluggable local-first OpenAI-compatible inference, out-of-tick guarantee, validated-paths-only actuation, Go `fl-director`/`fl-ops` services; Phase 3 close-out re-scope — #468 → Phase 4, renderer advancement → Phase 8 under #597, Phase 8 milestone renamed "Rendering & Alternative Backends"); a new "AI & Agentic Services" reference table + OpenSSL crypto-row fix in `docs/references.md`; a "Dynamic AI" prior-art section; planned `[ai.provider]`/`[ai.mcp]` config surface in `docs/fl-server-config.md`; a "Dynamic world, optional intelligence" design pillar in `docs/design.md`; `docs/modding/ai.md` repointed from the superseded #33 to #584/#413/#412; `.github/labels.yml` synced with the live label set (+ new `component: ui`). Backlog restructured in the same pass: 16 new epics (#583–#598), ~60 new sub-issues, all Phase 3–9 orphans re-homed, legacy umbrellas #33/#34/#42/#50 closed as superseded
- **network**: **GameNetworkingSockets transport behind `INetwork`** (Epic L; #507 + #508 + #509). Adds a second, encrypted UDP transport backend (`platform/net/GnsNetwork`, GNS v1.6.0) alongside `enet6`, selected at runtime by a new backend factory (`createNetwork(TransportKind)` in `platform/net/NetworkFactory.h`) behind the unchanged `INetwork` HAL. `fl-server` picks the transport via `[network] transport = "gns"|"enet"` (default `gns`) or `--transport <gns|enet>`; the game client uses GNS for internet multiplayer and enet6 for single-player (the embedded `LocalServer` spawns `fl-server` with `--transport enet`), closing the direct-`ENetNetwork` HAL leak in `Game.cpp`. GNS gives encrypted transport (curve25519 + AES-GCM, on by default; `[network] allow_insecure` maps to `AllowWithoutAuth`), mature congestion control, and 128+ connection headroom. `net_check`/`bot_swarm` intentionally stay on enet6 (the cross-swap regression instrument; the load-test runners force `--transport enet`). Built with `-DFL_ENABLE_GNS=ON` (default; `OFF` yields a lean enet6-only build); crypto = **OpenSSL** and protobuf is **system-preferred with a FetchContent fallback** (both reverse the #506 spike's libsodium/pure-FetchContent picks — GNS rejects libsodium AES on ARM/Apple Silicon, and CMake's export-file `include()` guard blocks the pure-FetchContent protobuf handoff; see the decision record). Two new `INetwork` server-tuning virtuals (`setBandwidthLimit`, `setPreHandshakeRateLimit`) remove the concrete-type downcasts in `fl-server`, plus `setAllowInsecure`. Covered by `test_gns_network` (loopback connect / reliable+unreliable send / broadcast / disconnect / link-stats / multi-instance refcount), `test_network_factory`, and `[network]` cases in `test_server_config`. See `docs/gns-backend.md` (#507)
- **docs**: network transport selection spike resolved (Epic L, #506). New `docs/transport-selection.md` evaluates **GameNetworkingSockets** vs a bespoke UDP layer vs hardening `enet6` against the `INetwork` HAL contract and **selects GameNetworkingSockets** (BSD-3) as the strategic 128+ transport, with `enet6` (MIT) retained behind a backend-selecting factory for LAN/single-player/low-count servers so the heavy Protobuf/crypto chain stays out of those build paths; crypto backend = libsodium (ISC). The selection is justified on encryption + mature congestion control + connection-count headroom — **not** throughput (#505 showed the transport is not the 96–256 ceiling). No `INetwork` interface change is anticipated and the wire format (`kProtocolVersion = 1`) is unchanged; implementation follows in #507 (backend + factory, closing the game-client direct-`ENetNetwork` HAL leak), #508 (encryption/DTLS + LAN-beacon/RCON reconciliation), and #509 (FetchContent GNS + Protobuf + libsodium + CI). Records a dated decision record in `docs/architecture.md` (locked-decisions table updated) and updates `docs/roadmap.md` (Epic L), `docs/network-protocol.md` (new Transport section), and `docs/references.md`. See `docs/transport-selection.md` (#506)
- **engine**: entity-pool + `SpatialIndex` scaling validation to thousands of entities — a server-side load-spawn affordance (`[world] test_spawn_ai_count` / `test_spawn_spread_km` / `test_spawn_agl_m`; **a testing affordance, not a capacity guarantee**) pre-spawns N cheap loiter-AI entities in a phyllotaxis spread so the per-tick data structures can be characterised past the ~128-player `bot_swarm` range without needing that many real clients. Adds an `entity-scale` `scale_gate.py` profile (advisory, **never baselined** — its entity×worker sweep would corrupt the KB/s baseline) driven by new `run_loadtest.sh`/`.ps1` env (`FL_TEST_SPAWN_AI` / `FL_TEST_SPAWN_SPREAD_KM` / `FL_SIM_WORKER_THREADS` / `FL_SNAPSHOT_BUDGET`) and reference-env sweep dims (`ENTITY_COUNTS` / `SIM_WORKERS`); a `bot_swarm --assert-min-entities N` gate hook (fails if `server_tick.entities < N`); and isolated `[.][scale]`-hidden microbenchmarks in `tests/test_entity_scale.cpp`. The run matrix + findings are recorded in `docs/entity-scale-characterization.md`: the pool and index are **not** the cliff at scale (integrate/ai/collision stay cheap) — the dominant per-tick cost is the per-peer Serialize phase, which the data-parallel passes scale down with worker count. Covered by `tests/test_load_spawn.cpp` (pure spread geometry) + new cases in `test_entity`/`test_spatial_index`/`test_world_broadcaster`/`test_server_config`/`test_bot_swarm`/`test_scale_gate.py`. See `docs/entity-scale-characterization.md` (#573)
- **engine**: graceful tick-overrun handling — a server-wide overrun **governor** (`engine/net/TickGovernor`, a pure AIMD policy) that, when the authoritative tick's measured wall-time exceeds its fixed-step budget under load, sheds work to pull the tick back under budget rather than spiralling or silently dilating time. It drives a single `loadFactor` from an EWMA of per-tick wall-ms (not the laggy 60 s p99) and folds three composing levers on top of the per-client `CongestionController`: server-wide **snapshot send-rate decimation** (`max(perPeer, governor)`), **per-client byte-budget scaling**, and **AI-sample decimation** for non-player entities (the integrate pass is never decimated — fixed-dt stability; auto-reducing the sim Hz is deliberately rejected). The skip predicate `(tickIndex+idx)%stride` with per-entity input caching is serial-equivalent across worker counts (bit-identical, TSan-clean). `GameLoop` keeps the catch-up cap as the spiral backstop, now configurable (`[world] max_catchup_ticks`), factored into a pure `clampCatchupTicks` helper, with the discarded count exposed via `totalDroppedTicks()` (fl-server logs a `Warn` when it rises). A healthy/disabled/never-overrun server holds `loadFactor == 1` — byte-for-byte the prior behaviour, no wire-format change. Observability: `status`/`tickstats` show load %, `--metrics-json` (`ServerTickReport` schema v2) adds `load_factor` + `dropped_ticks`. Config: `[world] overrun_governor_enabled` / `overrun_high_watermark` / `overrun_low_watermark` / `overrun_min_snapshot_hz` / `overrun_max_ai_stride` / `overrun_budget_floor_bytes` (all hot-reloadable via `reload_config`) + `max_catchup_ticks` (restart). Covered by `test_tick_governor` + `[overrun]` cases in `test_world_broadcaster` (incl. an AI-decimation serial-equivalence test) and additions to `test_loop`/`test_tick_profiler`/`test_server_tick_report`/`test_server_config`/`test_admin_console`. See `docs/server-job-system-design.md` (#514)
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

- **docs**: work-tracking conventions from the 2026-07-01 global prioritization pass recorded in `docs/project-management.md`: the board **Order** field's two-layer convention (a unified epic implementation-order sequence `1–N` + phase-banded work-item numbers, band = the item's milestone, blocks in epic-sequence order), the **epic finish-phase milestone** rule (an epic carries the milestone of its last open sub-issue and is re-homed forward rather than closed with open subs — the #494/#496/#588–#592 precedent), same-org **cross-repo sub-issues** (epic #54 parenting the `fl-base-pack` content issues), and the milestone **due-date policy** (dates only on the active phase and externally anchored gates). The CLAUDE.md project-overview paragraph is updated to match, including the newly superseded pre-epic stubs #130/#144/#145/#249
- **engine**: `EntityPool::forEach` is now **O(liveCount)** — it walks a dense live-index list (O(1) swap-remove on `free()`) instead of scanning the whole slot vector, so the per-tick `SpatialIndex` rebuild no longer pays for dead slots under high spawn/reap churn. Iteration order is now free-history-dependent; all consumers are order-independent (guarded by the `test_world_broadcaster` serial-equivalence + TSan tests). `SpatialIndex` gains a runtime-configurable cell size (`[world] spatial_cell_size_km`, `0` = auto = `clamp(drawDist/32, 500 m, 10 km)`; `WorldBroadcaster::setSpatialCellSize`) — a cell far smaller than the draw distance explodes the `queryRadius` cell count — and a recycled-buffer `clear()` that reuses cell vectors across ticks (no per-tick bucket reallocation in steady state). No wire-format change (#573)
- **network**: per-peer snapshot assembly now runs data-parallel. The Serialize phase of `WorldBroadcaster::onTick` builds each peer's interest-managed, delta-compressed `MsgWorldSnapshot` on the injected `JobSystem` (a new `runPeerPass` over peers, mirroring the per-entity passes) instead of one sequential loop: a serial gather resolves each sending peer's per-`peerId`-isolated state pointers, workers build per-peer buffers in parallel over a shared read-only entity map + `SpatialIndex`, then the sim thread flushes them (`m_net.send` stays sim-thread-owned). The per-peer build performs no cross-peer writes or RNG, so the output is byte-identical across worker counts (and to the previous serial path) — no wire change; validated by a new serial-equivalence test in `test_world_broadcaster` (including a mid-run kill exercising the parallel despawn path) and the `tsan` preset. Lifts the single-threaded per-peer snapshot ceiling at 128+ players (#512)
- **network**: client-acked delta baselines replace the fixed `baseline_interval_ticks` re-sync. The server now drives full-vs-delta off what each client has actually acknowledged (the last-received snapshot tick the client already echoes in `MsgClientInput`/`MsgHeartbeat`): an entity is re-sent as a full record every tick until the peer acks the tick its full streak started on, then converges to deltas. This removes the globally-synchronized periodic full-resync spike across all peers and recovers a dropped full in ~1 RTT instead of up to 2 s, with no wire-format change. The client now ignores out-of-order/duplicate snapshots so its echoed tick stays a monotonic ack. The `[world] baseline_interval_ticks` config field, its `reload_config` handling, and `WorldBroadcaster::setBaselineInterval` are removed (a stale key in `server.toml` is silently ignored); the per-entity `kSnapshotRetentionTicks` force-full (interest-out re-entry) is retained. Covered by new cases in `test_world_broadcaster`/`test_client_net_event_handler` (#517)
- **network**: selective-ack snapshot identity precision — the client→server ack (`MsgClientInput`/`MsgHeartbeat`) now carries a 32-bit `ackMask` bitmask of recently **decoded** snapshot ticks alongside the high-water `tickIndex` (new pure `engine/net/AckWindow.h`). The server confirms full-vs-delta against the *specific* tick a full record was sent in rather than a high-water mark, closing the #517 residual where acking a later tick could falsely confirm a full the client never decoded (which briefly made the entity invisible until the retention force-full). This retires the #517 "deferral guard" (now redundant). No wire-size or `kProtocolVersion` change — `ackMask` reuses the two messages' former reserved padding; `bot_swarm` fills it so the scale-gate bandwidth baseline is unaffected. Limitation: a snapshot received but not fully decoded (mid-bitstream truncation) still sets its ack bit — rare within the per-client MTU-fragment budget, with retention as the backstop. Covered by new `test_ack_window` + `[identity-ack]` cases in `test_world_broadcaster`/`test_client_net_event_handler` (#566)
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
