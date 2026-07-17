# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **docs**: Authoritative "Content Pack Linking Exception" text in `GOVERNANCE.md` — defines the Vendorable Interface Set (`IContentPack.h`, `AssetTypes.h`, `TrustLevel.h`), what "link against" permits, and what remains GPL; `IContentPack.h`'s header pointer now resolves (#663)
- **sensor**: Optional `[sensor] role` (`aircraft`/`seeker`, default `aircraft`); tooling-only. A `seeker` head is exempt from the non-emitting-track-lobe warning (#902)
- **ai**: `DynamicLoiterController` orbits a **moving** entity — re-centers the loiter circle on the target's live position each tick and matches its altitude; exposed as the `dynamic_loiter` AI behavior, and the `escort` template now tracks the moving escortee instead of its spawn point (#464)
- **engine**: `IWindow::showFolderDialog(title, defaultLocation)` — native folder picker for content-pack `configure()` UIs; non-pure with a nullopt default (implementors/mocks keep compiling), SDL3 backend via `SDL_ShowFileDialogWithProperties`. Adds a virtual to the `IWindow` vtable — rebuild native plugins against this header revision (#665)
- **tools**: `bot_swarm` samples server RSS periodically into an `rss_series` (report `schema_version` 4) and gates on the growth **trend** — `--assert-max-rss-slope-kb-per-min` fits the run's tail and fails a slow leak that stayed under the endpoint bound; wired into the `soak` scale-gate profile (#789)
- **network**: `SnapshotLastAckedSeqNum` snapshot TLV (`0x0105`) carries the exact `seqNum` the server last applied for a peer, so client-side prediction replays precisely the un-reflected inputs instead of approximating the window from `estimatedDelayTicks` — exact under high delay variance; additive, falls back to the delay-ticks estimate when absent (#427)
- **flight**: `MsgWeatherState` broadcasts the turbulence amplitude (grew 20→24 bytes, additive) so client-side prediction reproduces the server's per-tick turbulence **exactly** via the shared deterministic `weatherTurbulence(entityIdx, tickIndex, amp)`; prediction previously predicted zero turbulence and jittered on every gusty reconciliation (#426)

### Fixed

- **tools**: `validate-sensor` no longer warns "this sensor can never hold a lock" for a correctly-authored SARH seeker head (radar, `emitter = false`, track lobe, `role = "seeker"`); an aircraft radar with the same shape still warns (#902)

## [0.3.5] - 2026-07-17

### Added

- **flight**: Optional `[engine.idle_thrust]` deck — blends `idle → mil` across throttle instead of `0 → mil`; idle may be negative (ram drag). Additive, byte-identical when absent (#898)
- **flight**: Optional published-aero schema gaps from NASA TP-1538 — α-tabulated dampers (`cm_q_table`/`cl_p_table`/`cn_r_table`), `ixz_kg_m2` roll↔yaw coupling, `engine_ang_momentum` (He) gyroscopic term, `cn_da`/`cl_dr` cross terms, `cm0`, and speed-brake pitch/lift increments; all additive (#899)
- **flight**: `has_fbw` extends to full envelope protection — negative-g limiting against `min_g_structural` and an optional `[aero.limits] alpha_limit_deg` FLCS AoA cap (distinct from `alpha_stall_deg`), not just the positive-g limiter (#900)
- **entity**: Centreline single-engine subsystem — `[damage.subsystems.engine]` + `kEngineFailCenter` (total thrust loss, no yaw); snapshot engineFail field 5→6 bits (#901)
- **render**: Content meshes are authored in the standard glTF/Blender +Z-forward convention and rotated into the engine body frame (+X) on import (#906)

### Fixed

- **build**: The released Linux `fl-server` is statically linked against protobuf, so it runs on a clean machine (no `libprotobuf.so` dependency); CI + release gate on `ldd` self-containment (#905)
- **render**: `test_scene_renderer` palette-material test no longer fails in a release build — asserts the config-dependent placeholder-material contract instead of the Debug-only one (#897)

## [0.3.4] - 2026-07-16

### Added

- **game**: Instant Action + Free Flight main-menu entries — the first single-player slot launches the builtin skirmish (`builtin:sandbox`); Free Flight keeps the empty practice world (#40)
- **game**: Menu bypass — `--mission <id>` enters a single-player session directly and `--auto` enters Free Flight / Join Server, composing with `--connect`/`--observer` (#894)
- **mission**: `builtin:shape-gallery` — a compiled-in visual-verification scene for the per-category placeholder meshes (#886)
- **tools**: `tools/visual_check.sh` / `.ps1` — one-command visual verification: boots fl-server with the gallery, stages wreck variants via stdin `detonate`, and opens the game as an observer ghost (`--fly` for the armed pilot seat) (#894)
- **network**: fl-server `--mission` now also accepts a `.yaml`/`.yml` file path (builtin id → file → pack asset) (#894)
- **renderer**: Per-category builtin placeholder meshes with slumped wreck variants; the tetrahedron is removed and a spiky "Unknown" error beacon renders only in bug states (#886)
- **network**: `MsgEntityTypeDef` carries the entity's `ObjectCategory` + `ProjectileKind` ordinals (332 → 336 bytes, additive tail-append) (#886)
- **entity**: New `ObjectCategory::Structure` and a `ProjectileKind` vocabulary with an optional `projectile_kind` entity TOML key (#886)
- **entity**: Allowed-driven hardpoint compatibility — a station's `allowed` list drives store compatibility, so one pylon can mount mixed store types (#895)
- **mission**: Ground/ramp start — a mission object can spawn parked with `start: ground`, held by the parking hold until the pilot rotates (#885)

### Fixed

- **flight**: Flight integration no longer diverges for real aero decks — an energy-pumping transport term (explicit `−ω×v` tangent) and an inverted yaw moment sign made every flight-integrated pack aircraft depart from level flight and get silently reaped; the transport term is now an exact rotation, the yaw sign is corrected at the aero→engine seam, and a diverged entity logs an envelope-departure line (#891)
- **mission**: A `destroy(<player-slot>)` failure trigger no longer fires at 0.0 s before the pilot connects — player slots seed the evaluator as unoccupied and bind on the connect handshake (#884)
- **mission**: Airborne mission spawns no longer tumble at t=0 — the integrator is seeded with the spawn heading and a forward airspeed (optional `speed:`) (#883)
- **flight**: A cambered wing's negative trim alpha is no longer reported as a trim failure by fm-trim (#896)

## [0.3.3] - 2026-07-15

### Added

- **game**: Observer entity selection and view-from-entity camera — a spectator cycles live entities (Num1/Num2) and views any in Chase/Cockpit, labelled by type name + faction; a destroyed pick degrades to free-fly. Adds snapshot `factionIndex`, `MsgEntityTypeDef.name`, and a `MsgFactionDef` (0x10) name table (#860)
- **game**: Observer ghost-camera flow — `--observer` joins as a free-fly spectator with no ownship (no more stalled loading screen); a mid-session `set_role` switch wires/tears down prediction and the cockpit (#859)
- **network**: Camera-position interest for entity-less peers — `MsgClientInput` carries the client's camera eye so an observer or dead peer receives a meaningful, camera-centered snapshot (#858)
- **renderer**: Cockpit-interior mesh rendering — the ownship's `EntityDef::cockpitMesh` is drawn locked to the airframe in Cockpit view (#870)
- **renderer**: Builtin textured entity path — the placeholder mesh samples procedural base-color/normal/ORM maps via a raw-RGBA upload fallback, so texturing runs zero-pack (#867)
- **audio**: Procedural builtin music + default playlist — deterministic compiled-in menu/patrol/combat loops so music plays with no pack `playlist.toml` (#865)
- **mission**: Builtin sandbox mission — `--mission builtin:sandbox` runs a compiled-in skirmish (player + `builtin:fighter` wingman vs. bandits and a SAM) with no content pack (#868)
- **entity**: Builtin damage model — `builtin:debug-entity` gains a real 3-level `DamageDef` + subsystem table and a wreck-variant mesh, so progressive/directed damage runs zero-pack (#864)
- **script**: Embedded builtin Lua AI — `builtin:fighter` (patrol → intercept → engage, honest-sensing, fires) ships compiled into the engine via the `loadAIScript` seam (#866)
- **entity**: Builtin surface targets and threats — ground/naval/static targets plus shoot-back SAM and AAA emplacements (new `sam`/`aaa` behaviors), all zero-pack (#863)
- **engine**: Complete builtin weapon vocabulary — bomb, rocket pod, SARH missile, drop tank, and sensor pod join the builtins; adds `WeaponType::Fuel` and makes Fuel/Pod inert (mass+drag, never fire) (#862)
- **network**: Required-pack policy on the connect handshake — `[mods] required` + `[mods] required_policy` (`warn`/`refuse`/`allow_placeholder`) so a client learns which packs it lacks instead of silent placeholders (#872)

### Changed

- **engine**: Builtin particle + weapon-SFX preset registration moved into the engine (`registerBuiltinParticlePresets`/`registerBuiltinSfxPresets`), so any engine-linked frontend gets the named presets without re-registering them. No behavior change (#869)

## [0.3.2] - 2026-07-15

### Added

- **mission**: Mission & campaign runtime — engine-mission parser (schema owner; `validate-mission` delegates), faction-aware sim setup with real coalitions, scripted-bot `ai`/`route`/`loadout`, an objective/trigger evaluator, and single-player select→brief→fly→debrief (#584: #632/#854/#855/#633/#856/#634)
- **network**: Unified connect handshake — client-first `MsgConnectRequest` (role, entity type, pack manifest), observer role, and a configurable client-selectable aircraft (#853/#834/#857/#872)
- **tools**: Missions-as-integration-tests harness — `fl-server --mission-report` runs a mission headless to a JSON outcome, asserted in CI (#856)
- **render**: Mesh texture pipeline — meshes render with their authored PBR textures instead of flat grey (#833)
- **game**: Positional weapon release/impact audio + haptics, with byte-stable procedural fallbacks (#631)
- **engine**: The fire trigger works — guns/missiles/bombs/rockets resolve through a real weapons pass with replicated projectiles (#625)
- **engine**: IR missiles fly and think honestly — single-contact seeker over the shared detection machine, true PN guidance, segment fuze (#627)
- **engine**: Radar missiles fly on the shooter's radar — SARH/pre-pitbull support, pitbull go-active, loft (#628)
- **ai**: AI gunfights land hits — Picard-iterated ballistic lead + a guns-employment controller with trigger discipline (#462)
- **engine**: Bombs and unguided rockets fly real release ballistics — downward ejection, CEP dispersion, per-round mass/drag (#629)
- **flight**: Ballistic vehicles are real flight models (`type = "ballistic"`) with US-Std-1976 atmosphere to 86 km (#354)
- **ai**: Ballistic missiles fly themselves — boost-phase guidance to an impact point, MIRV child deployment (#355)
- **entity**: The kill chain's damage half — one damage funnel with friendly fire, kill feed/stats channel, debrief numbers, pilot career log (#626)
- **entity**: Per-subsystem damage — optional `[damage.subsystems]` routing hits to engine/controls/avionics/hydraulics/fuel (#675)
- **entity**: Area-of-effect warhead detonation with linear falloff and a nuclear EMP ring (#356)
- **engine**: Entity-entity collision detection — sphere-sphere in the parallel tick, relative-speed damage (#630)
- **network**: Server-side lag compensation — player hitscan rewinds targets to the tick the shooter saw (#425)
- **engine**: The zero-pack sandbox ships armed — builtin cannon/IR/radar weapons + seeker heads (#440)
- **content**: Weapons are a real asset type — `WeaponRegistry` + a default loadout that costs the airframe mass and drag (#812)
- **content**: The missile endgame — kinematic flyout, all probability in the seeker, proximity-fuze geometry (resolves RFC #676)
- **ui**: The in-game aircraft manual is generated from the flight model, not hand-written (#821)
- **flight**: `[aero.limits]` is enforced — stall flag, load factor, FBW G-limiter, over-G damage (#816)
- **flight**: `[aero.cd_table]` — tabulated total clean drag; the parabolic polar cannot represent a real fighter (#820)
- **flight**: `[aero.controls] max_elevator_neg_deg` — asymmetric pitch travel (#822)
- **tools**: `validate-entity` — entity-def validator with `--pack` reference resolution through the real content system (#829)
- **tools**: `fm-trim` — derives an aircraft's performance from its flight model and gates it against the flight manual in CI (#817/#826)

### Changed

- **network**: ENet `MsgId` space reserves `0x00–0x1F` for ENet / `0x20+` for raw-UDP; `MsgConnectAck` grew 16→20 B (#853)
- **flight**: `EntityDef` owns mesh/cockpit wiring; the dead `[aircraft] mesh`/`cockpit` keys are gone (#813)
- **tools**: `validate-weapon --pack` validates weapons; the hardpoint↔weapon cross-check moved to `validate-entity` (#829)
- **docs**: The aircraft-likeness policy now governs reference imagery (public-domain/CC0 only; scale plans and cutaways are copyrighted traps) (#835)

### Fixed

- **game**: Flying a mission from the select flow crashed on Fly — a null `LoadingScreen`; a session now starts on any entry into Loading (#876)
- **weapon**: Loadout default-selection looped a `uint8_t` index against `stations.size()` — a CodeQL comparison-with-wider-type defect (#878)
- **netcode**: The client flew a different flight model from the server, silently and permanently (#811)
- **content**: Def cross-references resolved by filename, not id — every pack aircraft's sensors silently reached the builtin eyeball (#810)
- **flight**: The body-frame velocity update was missing the ω × v transport term, so a pitching aircraft developed no AoA and could not pull g (#816)
- **flight**: `fm-trim` declared every aircraft unflyable above ~8 km — the back side of the power curve (#825)
- **engine**: Hardened every TOML integer read against a float→int UB in toml++ (#824)
- **render**: A failed mesh load silently became the builtin placeholder and logged nothing; now warns once per cause (#832)
- **game**: The client and server resolved `mods/` differently, so a server-loaded pack was invisible to the client (#831)
- **docs**: `ai.md` documented `pitch_error_from_alt` with the wrong signature; the example is now an executed test (#830)
- **tools**: Flight-model plausibility bands excluded the entire light-fighter class (#815)

## [0.3.1] - 2026-07-13

### Added

- **test**: Honest-sensing acceptance scenarios (#693)
- **docs**: Sensor, signature and detection authoring guide (#694)
- **engine**: Weather and night degrade detection, per sensor channel (#209)
- **ai**: Sensing-gated conditions, honest targeting, and the template migration (#690)
- **ai**: Lua `detected_contacts()` — scripted AI reads honest contacts instead of ground truth (#691)
- **engine**: Server-side difficulty — the `[ai]` preset reaches the sim tick (#682)
- **netcode**: Sensing pass — `SensorSystem` + `TickPhase::Sensing` in the parallel tick (#685)
- **engine**: Dual-lobe detection math with deterministic probability rolls (#684)
- **entity**: Signatures, sensor suite and per-unit AI tuning on entity definitions (#680)
- **content**: Sensor definitions as a first-class content-pack asset type (#679)
- **tools**: `validate-sensor` CLI (#679)
- **ai**: Scripted wingman command grammar, the formation/command hierarchy it is issued against, and the in-flight radio menu (#610)

### Changed

- **ai**: `attack_my_target` now designates from the lead's contact table, closing the seam #610 cut (#610)
- **tools**: Sensing phase in the scale gate; `kServerTickSchemaVersion` frozen at 6 for the rest of primary development (#686)
- **ai**: Bundled `AiTickContext` replaces the growing parameter tail on the controller seam (#681)
- **docs**: Decision record: probability of detection gates acquisition; geometry maintains the contact (#684) (#684)
- **server**: Players are now stamped with a faction on connect (`[world] player_faction`, default 1) (#610)
- **tools**: `validate-weapon` CLI + pack loadout cross-check (#624)
- **content**: Weapon TOML parser + entity hardpoints (#623)
- **network**: Engine-layer snapshot payload compression + a GNS datagram-coalescing knob (#775)
- **network**: Wire-byte accounting — measure what each transport actually costs on the socket (#772)
- **network**: 128-client scale gate on the GameNetworkingSockets transport (#649)
- **ai**: Local-provider AI evaluation harness (`tools/ai_eval`) + spike resolution (#599)
- **engine**: Heavier-AI-mix + projectile-churn load-generator affordances (#580)
- **ci**: GameNetworkingSockets build legs on Windows and macOS (#653)
- **network**: Synthetic congestion scale-gate profile (#714)
- **ci**: Synthetic tick-overrun scale-gate profile (#574)
- **tools**: bot_swarm trace replay + weighted pattern mix (#560)
- **server**: Overrun-governor interest-radius lever (#726)
- **network**: Shared-origin snapshot quantization (#725)
- **flight**: Local-level frame utilities (`LocalFrame.h`) (#470)
- **server**: Self-reported process RSS in the tick metrics, plus a `bot_swarm` soak leak gate (#707)
- **ai**: Entity faction/team tagging with an `AnyHostileEntityWithinRange` StateMachineController condition and a `spawn --faction <n>` flag (#465)
- **terrain**: Cube-sphere tile addressing and geometry core (`CubeSphere.h`) (#469)
- **terrain**: Cube-sphere tile mesh builder (`buildTileMeshGlb`) (#471)
- **engine**: Load content-pack entity definitions into the fl-server type registry at startup (#683)
- **terrain**: Bundled coarse global base terrain support (#474)
- **tools**: `gen_terrain_tiles.py` — GDAL cube-sphere quadtree tile generator (#473)
- **ai**: Decision: the natural-language wingman is a GPU feature; CPU servers get the scripted grammar (#769)
- **docs**: Full dynamic-campaign format specification (#183)
- **docs**: Decision record locking the unified sensor vocabulary and contact track model (#678)
- **ai**: Resolved spikes #604 (mission YAML generate/validate/repair) and #609 (NL → command-grammar intent) (#609)
- **ci**: Regenerated the committed scale-gate baselines on the reference runner after #775 changed the byte profile of every leg (#775)
- **ci**: GameNetworkingSockets is now the primary scale-gate profile, and the whole performance characterisation was re-derived on it (#773)
- **ai**: Measured the #599 latency budgets on the CPU-only 8-core reference instance (#599)
- **ai**: `ai_eval.py` gains `--merge-system`, folding the system prompt into the user turn for chat templates that have no system role (#599)
- **ci**: Regenerated the scale-gate `downstream_kbs_per_client` baseline on the 8-core reference runner (#766)
- **content**: `IContentPack::resolveTerrainChunk` renamed to `resolveTilePath` for cube-sphere tiles (#473)
- **terrain**: Cube-sphere quadtree terrain streamer (#472)
- **game**: Radial HUD artificial horizon + camera up (#479)
- **ai**: AI guidance now works on the local tangent (ENU) frame at the entity's position instead of the world XZ/Y frame
- **engine**: Physics LOD design record (#575)
- **flight**: Radial ground floor and collision (#477)

### Fixed

- **tests**: `test_loop` runs serially under ctest — its wall-clock rate assertions were measuring runner load, not the game loop (#807)
- **tests**: The ctest suite is now parallel-safe, and runs in parallel by default (#787)
- **network**: GNS clients always reported an RTT of 0 (#649)
- **game**: Client-side prediction was inert in the wired-up game (#755)
- **ci**: release workflow now actually attaches the Windows/Linux/macOS binaries to the GitHub Release

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
