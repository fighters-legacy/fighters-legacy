# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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

- **content**: `IContentPack::resolveTerrainChunk(terrainId, chunkX, chunkY, lod)` renamed to `resolveTilePath(terrainId, face, level, i, j, TileLayer layer)` (#473), un-folding the cube-sphere face from the terrain id and adding an explicit `fl::TileLayer` (Height / LandCover / Satellite). The pack path convention becomes `terrain/<id>/f<face>/l<level>/tile_<i>_<j>{.png,_lc.png,_sat.ktx2}` (replacing the transitional `f<face>/lod<level>/chunk_<iiii>_<jjjj>.png`). `AssetManager`, `FolderContentPack`, and `TerrainStreamer` updated; the satellite layer path is reserved but not yet generated
- **terrain**: Cube-sphere quadtree terrain streamer (#472) — `TerrainStreamer` rewritten from the planar ring-LOD chunk grid onto the CubeSphere `TileKey` quadtree: uniform 129×129 tiles, screen-space-error refinement to `maxTileLevel` (12 ≈ 30 m/quad), 2:1 edge balance + mesh skirts (crack rule), LRU residency cap, per-`{TileKey, layer}` async loads (height + optional land-cover; `surfaceAt` is real). New radial dvec3 query API (`heightAt`/`heightReadyAt`/`surfaceAt` on full world positions — headless-capable for authoritative server physics) with transitional near-side `(x, z)` overloads until the radial ground floor (#477). Procedural terrain now samples a global-sphere 3D FBM (`generateProceduralTile`) — seamless across faces and bit-identical between server and client. The planar `buildTerrainMeshGlb`, `generateProceduralChunk`, and the chunk-grid `TerrainManifest` fields were removed; `buildTileMeshGlb` gains an optional skirt; `terrain-chunk-io gen-procedural` takes `--face/--level/--i/--j`. Tiles bake the planet radius at generation time, so `setPlanetRadius` now flushes all resident tiles on a radius change, and both fl-server and the game client apply the radius before the first tile is streamed (fl-server before spawn priming; the client gates streaming on `MsgConnectAck`)
- **game**: Radial HUD artificial horizon + camera up (#479). The HUD attitude (pitch/heading/bank), the new artificial-horizon line, and the Free/Chase camera "up" vector are now computed on the local-level frame (`LocalFrame.h`) at the entity/camera position — radial up on a spherical planet — instead of assuming world +Y is up and world-XZ is the horizon plane. They stay correct far from the world origin (and are unchanged near it, since ENU ≈ world frame there). Adds `headingOf`/`bankOf` to `LocalFrame.h`, a `PTCH` readout + horizon line to `FlightHud`, and threads the server's `planetRadiusKm` (from `MsgConnectAck`) into `FlightHud::update`, `WindshieldRain` roll, and `CameraInput`
- **ai**: AI guidance now works on the local tangent (ENU) frame at the entity's position instead of the world XZ/Y frame, so pursuit/loiter/waypoint and every autopilot controller steer correctly far from the world origin (previously AI banked the wrong way / flew sideways away from origin). `Guidance.h`'s `horizontalHeadingError` and `pitchErrorFromAlt` take the planet radius (default Earth) and build on `LocalFrame.h`; `IEntityController` gains a `setPlanetRadius()` seam that `WorldBroadcaster` wires from the world's configured radius. Near-origin behavior is unchanged (#478)
- **engine**: Physics LOD design record — reduced-rate integration for distant AI, conditional go with integrate-bound trigger criterion (#575)
- **flight**: Radial ground floor and collision (#477). The authoritative physics floor was planar — it compared `pos_world[1]` against a world-Y ground elevation and snapped world-Y — so an aircraft circling the planet at constant world-Y never contacted the surface, and `(x, z)` was not a unique surface key on a sphere (the antipode aliases). Ground contact now compares the geodetic (MSL) altitude against the terrain radial elevation and snaps along the local radial up (new `IGravityField::geodeticUp`, defaulted to world +Y, `CentralGravityField` returns the radial direction), so collision is correct anywhere on the planet and bit-identical to the old planar clamp at the world origin. `groundElev` passed to `FlightIntegrator::step` is now the terrain elevation above the datum (`TerrainStreamer::heightAt(dvec3)`); the ground-query plumbing (`WorldBroadcaster::setGroundElevationQuery`, `ClientPrediction::HeightQuery`, fl-server spawn-priming) is now `dvec3`. The HUD `ALT`/`AGL`, the F3 debug-overlay AGL, the haptics GPWS AGL, and the free-camera ground clamp now read radial (`geodeticAltitude(pos)` / `− terrain radial elevation`) instead of world-Y — fixing the far-from-origin ALT/AGL inconsistency (`AGL > ALT`) surfaced by the #756 flight test. The transitional near-side `(x, z)` `TerrainStreamer` height/surface overloads were removed; fl-server maps its planar `(x, z)` spawn config onto the sphere's near side inline

### Fixed

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
