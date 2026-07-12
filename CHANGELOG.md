# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **network**: 128-client scale gate on the GameNetworkingSockets transport (#649). GNS has been the **default internet transport** on all three platforms since #507, yet every scale-gate leg was deliberately enet6 (`bot_swarm` is the enet6 regression instrument) — so the transport most players actually use had never been scale-tested. `bot_swarm` gains `--transport enet|gns` (**default `enet`**, so every pre-existing profile is byte-identical) and now links the `platform-net` facade instead of `platform-enet`; `run_loadtest.sh`/`.ps1` gain `FL_LOADTEST_TRANSPORT`, which pins **both ends** to the same backend. New `gns` scale-gate profile (128 clients, idle/weave/aggressive — mirrors `reference` for direct comparison) and a reference-runner workflow leg built with `FL_ENABLE_GNS=ON`, in the `nightly` set. **A GNS run can never silently degrade into an enet6 run** — that would be a gate that lies: `bot_swarm` refuses `--transport gns` in an enet6-only build (rather than taking `createNetwork`'s convenience fallback, via the new `transportAvailable()`), the report records the backend actually spoken (`transport`, swarm report **schema v3**) and the gate fails a mismatch, and the workflow asserts `FL_ENABLE_GNS:BOOL=ON` after configure (`cmake/dependencies.cmake` force-disables GNS *silently* when OpenSSL/protobuf are absent — the trap #653 hit). The `gns` profile is **not baselined**: GNS's encryption + framing means its byte profile is legitimately not enet6's. **Measured at 128 clients on the reference box: GNS admits 128/128, holds 60 Hz, and server tick p99 is ~7–8× *lower* than enet6** (1.47/1.51/1.73 ms vs 5.48/12.62/12.60 ms, same box and session) — ENet does its per-packet send work inline on the sim thread inside the serialize phase, while GNS hands off to its own service thread. Phase breakdown (weave): enet6 `serialize` 8.03 ms of an 8.20 ms tick (98%) vs GNS 0.81 ms of 1.01 ms (80%) — so **~90% of what the serialize phase cost was ENet's inline send, not the snapshot pipeline**, whose real cost is ~0.8 ms at 128 clients. Serialize remains the dominant phase on GNS, so the phase-routing clauses of the #572/#575 trigger criteria still hold; what changes is the *magnitude* — on the default transport the tick runs ~16x under its 16.6 ms budget rather than ~2x, so those triggers are much further away and any encode optimisation justified against an "8 ms serialize phase" is sized against a number that is really 0.8 ms on the transport that ships
- **ai**: Local-provider evaluation harness + spike resolution (#599) — `tools/ai_eval/`, a stdlib-only harness that measures latency, structured-output reliability and task correctness of any **OpenAI-compatible** endpoint (Ollama, `llama-server`, vLLM, LiteLLM, a hosted API) on the initiative's three workloads: `intent` (utterance → wingman command, 2 s budget), `mission` (brief → mission YAML, judged by the **real `validate-mission` binary**, scored pass@1 and pass-after-one-repair — the director's generate→validate→repair loop), and `ops` (metrics snapshot → root cause + actions, checked against an **action allowlist**). Suites are data (`suites/*.json`), so re-pointing `intent` at the final wingman grammar once #610 lands is a file edit. **CI never requires a model** — the harness is not wired into `ctest`; only its pure scoring logic is unit-tested (`tests/test_ai_eval.py`, 48 cases, zero network calls). Findings recorded in `docs/ai-provider-evaluation.md`: **~9B is the floor** at which the workloads work (96 % intent accuracy, 100 % validate-clean missions; 1.5B collapses), `qwen2.5-coder:14b` is the recommended default, and **ops triage is not ready for autonomy** (no model above 75 % root-cause accuracy; congestion systematically misread) — `fl-ops` stays at `observe`/`recommend`
- **engine**: Heavier-AI-mix + projectile-churn load-generator affordances (#580) — the #573 load-spawn deliberately used cheap static loiterers (isolating pool+index cost), leaving the AI phase and spawn/reap churn unstressed. New `[world] test_spawn_ai_mix` assigns the pre-spawned entities a weighted controller mix (`"loiter:60,pursuit:25,patrol:15"`, deterministic per-index assignment; `pursuit` = `EntityManager::get()` on a moving target each tick, `patrol` = a `StateMachineController` whose `AnyEntityWithinRange` transitions run `SpatialIndex::queryRadius()` every tick — the path the overrun governor's AI stride decimates). New `[world] test_projectile_rate`/`test_projectile_ttl_s` run a projectile-churn generator (short-lived entities spawned at the configured rate, killed after the TTL) stressing the `EntityPool` free-list, the O(liveCount) `forEach`, and the `SnapshotDespawn` TLV path. Runner env knobs `FL_TEST_SPAWN_MIX`/`FL_TEST_PROJECTILE_RATE`/`FL_TEST_PROJECTILE_TTL_S` + a new advisory `entity-churn` scale-gate profile. Testing affordances, NOT capacity guarantees. Indicative debug deltas at 2000 entities/1 worker: `ai_ms` +12%, `tick_ms` +11%
- **ci**: GameNetworkingSockets build legs on Windows and macOS (#653) — GNS (the default internet transport on all three platforms) was Linux-only in CI; the Windows and macOS builds were unvalidated. Each platform now sources the pre-abseil protobuf 3.21.x line GNS v1.6.0 requires: Linux keeps apt `libprotobuf-dev`; macOS installs the pinned keg-only formula `protobuf@21` surfaced via `CMAKE_PREFIX_PATH` (+ `openssl@3` via `OPENSSL_ROOT_DIR`); Windows uses a new repo-root `vcpkg.json` manifest pinning `protobuf` 3.21.12#4 under the runner's vcpkg toolchain (`x64-windows-static-md` triplet to match the presets' CRT, with a binary cache keyed on the manifest). Because `cmake/dependencies.cmake` gracefully falls back to enet6-only when the deps are missing, the legs now assert `FL_ENABLE_GNS:BOOL=ON` in the CMakeCache after configure — a broken dependency setup fails the leg instead of silently passing. The `vcpkg.json` only takes effect under the vcpkg toolchain; local non-vcpkg builds ignore it. Sanitizer/CodeQL/scale-gate legs stay deliberately enet6-only
- **network**: Synthetic congestion scale-gate profile (#714) — CI-proves the #518 per-peer AIMD send-rate controller engages on a degraded link and recovers when it clears. New portable lossy UDP proxy inside `bot_swarm` (`LossyProxy` — clients relay through a local socket that drops `--degrade-loss` of datagrams and adds `--degrade-delay-ms` one-way delay inside a scheduled `--degrade-start`/`--degrade-duration` window; no `tc netem`/`NET_ADMIN`, all platforms; pure drop/delay policy in `LossyLink.h`). The server now tracks run-long congestion watermarks (`WorldBroadcaster::getCongestionTelemetry()`, frozen while no peers are connected) exported as `congestion_min_send_hz` / `congestion_recovered_send_hz` / `congestion_max_loss` in `--metrics-json` (**`ServerTickReport` schema v5**). Two new `bot_swarm` asserts — `--assert-congestion-engaged-hz` (fails if the min send rate never fell to the threshold) and `--assert-congestion-recovered-hz` (fails if it didn't climb back) — express engaged-then-recovered from the single end-of-run metrics read. New non-baselined `congestion` scale-gate profile (reference/nightly tier; 5% loss + 100 ms delay — the delay is the deterministic engage trigger, and 5% stays below ENet's own peer-timeout threshold). Verified end-to-end both ways: degraded run engages to the 10 Hz floor and recovers to 60 Hz (PASS); a healthy-link run fails the engaged assert
- **ci**: Synthetic tick-overrun scale-gate profile (#574) — CI-proves the #514 graceful overrun governor engages and sheds under load rather than spiralling. New `overrun` scale-gate profile (reference/nightly tier) runs the governor ON via `FL_LOADTEST_GOVERNOR=1` (new `run_loadtest.sh`/`.ps1` env knob) and deterministically overloads a serialize-bound tick with `test_spawn_ai=5000` at `sim_workers=1`. Two new `bot_swarm` asserts — `--assert-max-load-factor` (fails if the governor never engaged, i.e. `load_factor` stayed 1.0) and `--assert-max-dropped-ticks` (the graceful-not-spiral property) — both with a negative-disabled sentinel since 0 is a real value. No new server plumbing (reads the existing `server_tick.load_factor`/`dropped_ticks`); `baselined: false` so the shed KB/s never touches the committed bandwidth baseline. Wired through `scale_gate.py` + `scale-gate.yml` reference tier; covered by `test_bot_swarm` + `test_scale_gate.py`
- **tools**: bot_swarm trace replay + weighted pattern mix (#560). Two additive load profiles for the `IFlightPattern` registry. `--pattern trace:<file>` replays a **recorded** real session as synthetic input (the immutable trace is loaded once and shared read-only across the swarm; each client phases its cursor by index and loops). `--pattern-mix "weave:80,aggressive:20"` builds a heterogeneous swarm with **deterministic** per-client assignment by weight proportion (no RNG; reproducible across runs/platforms); the mix spec is echoed in the report. Traces are recorded server-side: a new `[trace] input_trace_dir` config key and `trace_start [dir]` / `trace_stop` admin commands make `WorldBroadcaster` append every peer's **accepted** (post-validation) `MsgClientInput` to per-peer FLIT traces. New versioned, little-endian, self-describing FLIT format (`engine/net/InputTrace{Format,Writer,Reader}.h`) — documented for reuse by the Phase 4 replay epic (#588). `kProtocolVersion` unchanged
- **server**: Overrun-governor interest-radius lever (#726) — a fourth #514 shedding lever: under sustained tick overrun the governor scales each peer's effective interest radius (`draw_distance_km`) down toward `[world] overrun_min_interest_fraction` (default 0.5; 1.0 = lever off; hot-reloadable via `reload_config`), shrinking the visible set itself (interest query + scheduler ranking + encode) rather than trimming the encoded output. Entities leaving the shrunk radius are ordinary interest-out (client retention handles re-entry) — no despawn, no wire change; healthy servers are byte-for-byte unchanged. Observability: `getOverrunStatus().interestScale`, `status`/`tickstats` output, and `interest_scale` in `--metrics-json` (`ServerTickReport` schema v4)
- **network**: Shared-origin snapshot quantization — encode each entity once per tick (#725). Records are quantized relative to a shared grid-cell origin (not the receiving peer's position) and written peer-independently (absolute idx, byte-aligned), so per-peer snapshots are assembled by stitching pre-encoded blobs (memcpy) instead of re-quantizing per peer: `O(entities)` encode + `O(peers × visible)` memcpy. `MsgWorldSnapshotHeader` gains an origin table; ~+7% bandwidth for the O(entities) encode win. `kProtocolVersion` unchanged
- **flight**: Local-level frame utilities (`LocalFrame.h`) — `radialUp`, ENU basis, `localAltitude`, `headingTo`, and `pitchOf` on a spherical planet; header-only foundation for the radial physics floor and local-level nav/HUD (#470)
- **server**: Self-reported process RSS in the tick metrics (`ServerTickReport` schema v3: `rss_kb`/`rss_startup_kb` via `ProcessStats::currentRssKb()`) plus a portable `bot_swarm --assert-max-rss-growth-kb` soak leak gate wired into the `soak` scale-gate profile, replacing the Linux-only shell `ps` sampler (#707)
- **ai**: Entity faction/team tagging with an `AnyHostileEntityWithinRange` StateMachineController condition and a `spawn --faction <n>` flag; the `escort` template now ignores same-faction friendlies by faction rather than by geometry (#465)
- **terrain**: Cube-sphere tile addressing and geometry core (`CubeSphere.h`) — TileKey quadtree, tangent-adaptive face↔sphere warp, `tileToWorld`/`worldToTile`, and parent/child/neighbour topology; pure header-only math, foundation for the spherical-Earth terrain rewrite (#469)
- **terrain**: Cube-sphere tile mesh builder (`buildTileMeshGlb`) — true curved tiles with per-vertex world positions from `CubeSphere::tileToWorld`, curvature-correct normals from the surface tangent cross-product, optional `COLOR_0` (WorldCover class) / `TEXCOORD_0` attributes, and camera-relative rebase against a tile origin; CCW-from-outside winding (validate-mesh clean). Added alongside the planar `buildTerrainMeshGlb` pending the streamer rewrite (#471)
- **engine**: Load content-pack entity definitions into the fl-server type registry at startup, so pack entity types are spawnable on a dedicated server (malformed defs warn and skip; no protocol change) (#683)
- **terrain**: Bundled coarse global base terrain support (#474) — the engine can mount a small bundled cube-sphere base (`<dataDir>/base-terrain/`) as the lowest-priority content pack, so a zero-user-pack launch still serves real-Earth root tiles (`generateProceduralTile` fills finer near-camera detail; user packs override via the first-wins stack). New `loadBundledBaseTerrain()` (`engine/content/BundledBaseTerrain.h`) wired into the game and fl-server before `AssetManager` construction; it is a no-op (procedural-only, `hasPacks()==false`) when no base is present, detected via a face-root sentinel tile. New `tools/build_global_base.sh` builds the base from a global elevation source (GEBCO land+bathy ideal) + optional land cover via `gen_terrain_tiles.py`; the release workflow stages `base-terrain/` next to the binary when present. The base tiles themselves are produced/bundled out-of-band (not committed)
- **tools**: `gen_terrain_tiles.py` — GDAL cube-sphere quadtree tile generator (#473). Samples a global lat/lon DEM at each `TileKey(face, level, i, j)`'s 129² cube-sphere lattice (math ported from `CubeSphere.h`; direction→lat/lon per `Geodetic.h`) and writes `terrain/<id>/f<face>/l<level>/tile_<i>_<j>.png` height tiles (`elev+32768`, matching `generateProceduralTile`) plus optional nearest-neighbour `_lc.png` land cover; pole/antimeridian tiles handled via full-width latitude-band reads with longitude wrap. Pure-logic pytest (GDAL-free) + a GDAL synthetic-fixture CI smoke

### Changed

- **ai**: Measured the #599 latency budgets on the **CPU-only 8-core reference instance** — the box the initiative's acceptance gate actually names. The #599 sweep ran on an RTX 5080, leaving the assumption untested. **Two of the three budgets do not hold there**, and in both cases quality is unaffected (accuracy is a property of the model, not the host) — the accurate models are simply too slow. **Intent (2 s) fails:** no model is both ≥ 90 % accurate and inside budget (`gemma2:9b` = 92 % at 4.8 s p95; `qwen2.5-coder:14b` = 96 % at 3.3 s; the only model inside budget is `qwen2.5:3b` at 81 %). Latency is **prompt-eval dominated** — the cost is ingesting the ~190-token command grammar, not generating the ~12-token answer — so the levers are a shorter grammar (#610) and prefix caching, not a faster decoder. Recorded as an Epic O design fork in **#769** (GPU feature + scripted fallback / relax the budget / cut the prompt / make a small model accurate). **Mission (60 s) fails at the useful sizes:** 9B and 14B stay **100 % validate-clean** but take p95 71 s / 95 s, so the director (Epic N) must generate the next mission ahead of time rather than block on a synchronous call. **Ops fits** on CPU (p95 ≤ 24 s). Cold-loading a 14B costs **55 s** and idle models are evicted after 5 minutes, making a keep-warm policy (`OLLAMA_KEEP_ALIVE`) a deployment requirement whichever path is chosen. Recorded in `docs/ai-architecture.md` §2/§9 and a new CPU section of `docs/ai-provider-evaluation.md`, which keeps the GPU table alongside it
- **ai**: `ai_eval.py` gains `--merge-system`, folding the system prompt into the user turn for chat templates that have **no system role** (#599). Ollama *silently drops* a `system` message for such templates rather than erroring: served directly, `gemma2` never saw the command grammar, answered `unknown` to everything, and measured **35 %** on a suite where it is really a 92 % model — a wrong conclusion with nothing in the output to reveal it. LiteLLM (which fronted the original GPU sweep) merges the turn transparently, which is why the same model read 96 % there. The trap and its tell are documented in `docs/ai-provider-evaluation.md`
- **ci**: Regenerated the scale-gate `downstream_kbs_per_client` baseline on the 8-core reference runner (#766). The committed numbers predated #725's shared-origin encode-once (~+7% bandwidth, reviewed and accepted at the time as within tolerance), so measured runs had crept up to the +10% tolerance ceiling — `reference/aggressive` sat at 73.3 KB/s against a 73.7 limit, ~0.5% headroom, meaning the *next* byte-adding PR would have tripped a **false** regression against an already-accepted cost. New values (66.5–67.2 → 71.4–73.3) come from the reference VM; the hosted PR runner independently measured the same `pr/weave` figure to within 0.1 KB/s (71.4 vs 71.379), confirming the metric is machine-independent as documented. This is a re-baseline, not a capacity change: the hard gate is still 150 KB/s/client and runs sit at ~71–73, roughly 2× headroom. `docs/load-testing.md` now explains how to tell a real regression from an accepted-cost drift
- **content**: `IContentPack::resolveTerrainChunk(terrainId, chunkX, chunkY, lod)` renamed to `resolveTilePath(terrainId, face, level, i, j, TileLayer layer)` (#473), un-folding the cube-sphere face from the terrain id and adding an explicit `fl::TileLayer` (Height / LandCover / Satellite). The pack path convention becomes `terrain/<id>/f<face>/l<level>/tile_<i>_<j>{.png,_lc.png,_sat.ktx2}` (replacing the transitional `f<face>/lod<level>/chunk_<iiii>_<jjjj>.png`). `AssetManager`, `FolderContentPack`, and `TerrainStreamer` updated; the satellite layer path is reserved but not yet generated
- **terrain**: Cube-sphere quadtree terrain streamer (#472) — `TerrainStreamer` rewritten from the planar ring-LOD chunk grid onto the CubeSphere `TileKey` quadtree: uniform 129×129 tiles, screen-space-error refinement to `maxTileLevel` (12 ≈ 30 m/quad), 2:1 edge balance + mesh skirts (crack rule), LRU residency cap, per-`{TileKey, layer}` async loads (height + optional land-cover; `surfaceAt` is real). New radial dvec3 query API (`heightAt`/`heightReadyAt`/`surfaceAt` on full world positions — headless-capable for authoritative server physics) with transitional near-side `(x, z)` overloads until the radial ground floor (#477). Procedural terrain now samples a global-sphere 3D FBM (`generateProceduralTile`) — seamless across faces and bit-identical between server and client. The planar `buildTerrainMeshGlb`, `generateProceduralChunk`, and the chunk-grid `TerrainManifest` fields were removed; `buildTileMeshGlb` gains an optional skirt; `terrain-chunk-io gen-procedural` takes `--face/--level/--i/--j`. Tiles bake the planet radius at generation time, so `setPlanetRadius` now flushes all resident tiles on a radius change, and both fl-server and the game client apply the radius before the first tile is streamed (fl-server before spawn priming; the client gates streaming on `MsgConnectAck`)
- **game**: Radial HUD artificial horizon + camera up (#479). The HUD attitude (pitch/heading/bank), the new artificial-horizon line, and the Free/Chase camera "up" vector are now computed on the local-level frame (`LocalFrame.h`) at the entity/camera position — radial up on a spherical planet — instead of assuming world +Y is up and world-XZ is the horizon plane. They stay correct far from the world origin (and are unchanged near it, since ENU ≈ world frame there). Adds `headingOf`/`bankOf` to `LocalFrame.h`, a `PTCH` readout + horizon line to `FlightHud`, and threads the server's `planetRadiusKm` (from `MsgConnectAck`) into `FlightHud::update`, `WindshieldRain` roll, and `CameraInput`
- **ai**: AI guidance now works on the local tangent (ENU) frame at the entity's position instead of the world XZ/Y frame, so pursuit/loiter/waypoint and every autopilot controller steer correctly far from the world origin (previously AI banked the wrong way / flew sideways away from origin). `Guidance.h`'s `horizontalHeadingError` and `pitchErrorFromAlt` take the planet radius (default Earth) and build on `LocalFrame.h`; `IEntityController` gains a `setPlanetRadius()` seam that `WorldBroadcaster` wires from the world's configured radius. Near-origin behavior is unchanged (#478)
- **engine**: Physics LOD design record — reduced-rate integration for distant AI, conditional go with integrate-bound trigger criterion (#575)
- **flight**: Radial ground floor and collision (#477). The authoritative physics floor was planar — it compared `pos_world[1]` against a world-Y ground elevation and snapped world-Y — so an aircraft circling the planet at constant world-Y never contacted the surface, and `(x, z)` was not a unique surface key on a sphere (the antipode aliases). Ground contact now compares the geodetic (MSL) altitude against the terrain radial elevation and snaps along the local radial up (new `IGravityField::geodeticUp`, defaulted to world +Y, `CentralGravityField` returns the radial direction), so collision is correct anywhere on the planet and bit-identical to the old planar clamp at the world origin. `groundElev` passed to `FlightIntegrator::step` is now the terrain elevation above the datum (`TerrainStreamer::heightAt(dvec3)`); the ground-query plumbing (`WorldBroadcaster::setGroundElevationQuery`, `ClientPrediction::HeightQuery`, fl-server spawn-priming) is now `dvec3`. The HUD `ALT`/`AGL`, the F3 debug-overlay AGL, the haptics GPWS AGL, and the free-camera ground clamp now read radial (`geodeticAltitude(pos)` / `− terrain radial elevation`) instead of world-Y — fixing the far-from-origin ALT/AGL inconsistency (`AGL > ALT`) surfaced by the #756 flight test. The transitional near-side `(x, z)` `TerrainStreamer` height/surface overloads were removed; fl-server maps its planar `(x, z)` spawn config onto the sphere's near side inline

### Fixed

- **network**: GNS clients always reported an RTT of 0 (#649). `GnsNetwork::connect()` stores the outbound server connection in `m_clientConn` but never registers it in the peer maps (only the server's *accept* path populates those), while every other client-side path refers to that connection as peer 0 — so `connForPeer(0)` missed the map and `getPeerRtt(0)` / `getPeerLinkStats(0)` returned zero for every GNS client, where the enet6 backend returned real values. Found by the #649 load runs: the swarm's RTT column came back empty on GNS but not on enet6. Verified through the lossy proxy — with a +50 ms one-way delay a GNS client now reports 100 ms RTT (it correctly reports 0 on loopback, which is sub-millisecond against GNS's integer-ms ping)
- **game**: Client-side prediction was inert in the wired-up game (#755). `ClientPrediction::init()` was called from the `onConnect` lambda before `clientNet->connect()`, so it captured the player's entity idx/gen and the planet radius as their pre-`MsgConnectAck` defaults (`0`/`0`/`6371`). Since a valid live entity always has `gen != 0`, `reconcile()` never matched the player's snapshot entry and returned early on every snapshot — the ownship was never predicted or reconciled, and the non-Earth gravity field was never built. Prediction is now wired at the same post-ConnectAck one-shot gate that applies the server planet radius to the terrain streamer and camera (`assignedEntityGen != 0`), so idx/gen/radius are read after the handshake
- **ci**: release workflow now actually attaches the Windows/Linux/macOS binaries to the GitHub Release. The `Package` steps write `<artifact>.zip` at the workspace root, but `upload-artifact` globbed `dist/*.zip` (no match) under `if-no-files-found: ignore`, so every release since v0.2.x published with zero assets. Upload the root-level zip, harden `if-no-files-found: error` so a future path drift fails loudly, and flatten `download-artifact` into `dist/` (`merge-multiple: true`) so the release `files:`/attestation `subject-path:` globs resolve

## [0.3.0] - 2026-07-10

### Added

- **ai**: Lag pursuit controller for guns employment (#432) (#463)
- **ai**: Patrol_attack and escort StateMachineController templates (#430) (#466)
- **network**: Adaptive per-peer jitter buffer depth resizing (#467)
- **engine**: Engine-world foundation library (AlertLevel, FactionRegistry, AirspaceZone) (#491)
- **tools**: Headless bot-swarm load-testing client (#558)
- **engine**: Server tick-budget instrumentation (per-phase timing) (#561)
- **engine**: Data-parallel sim tick via engine-job job system (#510, #511) (#562)
- **network**: Quantized bit-packed snapshot codec + 3D interest culling (#563)
- **network**: Per-client priority/budget snapshot scheduler (#516) (#564)
- **network**: Client-acked delta baselines (#517) (#565)
- **network**: Adaptive per-client send-rate and congestion response (#518) (#568)
- **network**: Parallelize per-peer snapshot assembly (#512) (#571)
- **engine**: Graceful tick-overrun handling under load (#514) (#577)
- **network**: Selective-ack snapshot identity precision (#566) (#578)
- **engine**: Validate + scale entity-pool and SpatialIndex to thousands of entities (#579)
- **network**: Implement GameNetworkingSockets transport behind INetwork (#507) (#582)
- **audio**: Replace stb_vorbis with libvorbis and re-enable fuzz_ogg (#723) (#724)

### Changed

- Re-target roadmap to 128+ player multiplayer (#492)
- Add project-management methodology guide (#567)
- **network**: Select GameNetworkingSockets transport (#506) (#581)
- **ai**: Add dynamic world & agentic AI initiative (epics M-P) and phase 3 close-out re-scope (#655)
- Record board order and epic milestone conventions (#656)
- **engine**: Spatial sharding design record — defer with trigger criterion (#572) (#727)
- **roadmap**: Document the phase-gate release cadence (#737)

### Fixed

- **game**: Split multi-line admin responses into one console entry per line (#461)
- **content**: Pass assets root to ModLoader so native plugins load (#664)
- **renderer**: Center hud/menu text via HudAlign anchor field (#439) (#736)
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
