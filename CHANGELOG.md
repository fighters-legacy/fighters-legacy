# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **ai**: AI gunfights land hits (#462): `computeBallisticLead` (Picard-iterated ballistic lead — muzzle velocity, shooter-velocity carry, gravity drop along LOCAL down so the pipper stays honest planet-wide) and `GunsEmploymentController`, which steers the nose onto the lead point and holds trigger discipline — it fires only when the predicted miss distance at the target's range is inside the lethal radius, through the same honest `TargetView` contact path as every other controller; wired into the factory as `--ai guns <entityIdx>`
- **engine**: Radar missiles fly on the shooter's radar, honestly (#628): a SARH or pre-pitbull ARH shot is SUPPORTED — its picture is the shooter's own contact table (last-known state, never ground truth), launch requires the shooter to hold the target LOCKED, a support drop coasts on the head's `lock_hold_s` then goes dumb, and a dead shooter is a dropped shot; at `pitbull_nm` the ARH missile's own radar goes active (it starts EMITTING — the #529 RWR seam) with cued acquisition and flies autonomously from there; a loft bias climbs BVR shots into thin air until the loft range passes; and the pre-launch growl is replicated as the own-record `seekerLocked` flag driving a HUD LOCK annunciator — the server computes it from the same launch gate the missile itself will apply
- **engine**: IR missiles fly and think honestly (#627): a missile's seeker is a single-contact track over the SAME pure detection machine as every radar and eyeball in the game (`SeekerTrack` + `stepSeekerCheck` at the 10 Hz reference cadence, staggered per missile) — PoD gates acquisition, geometry maintains, a coast steers at where the target WAS, and reacquire inside the coast needs no new die; guidance is true proportional navigation (`a = N·Vc·λ̇`, N = 3.5, clamped by `max_g`) flown at the seeker's last-known state, never ground truth; the launch gate compresses the pre-launch growl to geometry (a target outside the acquisition lobe at release means the store flies dumb); the proximity fuze tests the flight SEGMENT so a Mach-4 closure cannot tunnel through the bubble in one tick; and the flare/chaff seam (#529) is a null-by-default hook that forces a coast, nothing more. Deprecated legacy seeker lobes are synthesized into real sensor defs so old packs keep flying
- **engine**: The sandbox ships armed (#440): compiled-in `builtin:cannon`/`builtin:ir-missile`/`builtin:radar-missile` (plausible-class, deliberately generic) with matching builtin IR/radar seeker heads following the one-sensor-vocabulary rule; the debug entity every peer spawns carries 1 gun + 2 IR + 2 radar stations, so the entire fire path is provable from a bare checkout; the client wires its station selector and a HUD `ARM <weapon> x<rounds>` line from the same shared builder, so server and client can never disagree about the sandbox loadout
- **network**: Server-side lag compensation (#425): player hitscan rewinds targets to the tick the shooter actually saw (`TransformHistory`, a 32-tick ≈533 ms post-integrate position ring), generation-checked so a recycled entity slot can never be hit through history; AI shooters and projectiles deliberately never rewind, and the clamp bounds the "shot from around the corner" effect
- **engine**: The fire trigger finally does something (#625): fire intent flows through `ControlInput` — one seam for players, C++ AI, and Lua scripts — into a serial weapons pass where `FireControl` validates station/ammo/rate/weapons-hold (the #610 `hold_fire` order gets its teeth) and guns resolve as deterministic-dispersion hitscan while stores become pooled 3-DOF projectile entities that replicate, despawn, and detonate through the same warhead pipeline as everything else. `MsgClientInput` grows to 56 bytes (absolute weapon-station selection — idempotent under loss), the own-entity snapshot record carries the LIVE loadout (selection, rounds, payload mass/drag — so client prediction feels a released store the same tick the server does), and cosmetic tracers/flashes/impacts ride a new unreliable `SnapshotEffects` TLV into a client effect router: a dropped packet loses sparks, never state
- **entity**: Area-of-effect warhead detonation (#356): blast damage with linear falloff through the same friendly-fire-gated funnel as everything else, a nuclear EMP ring (avionics kill at 4× the blast radius — electronics do not care about shrapnel range), and a `detonate` admin/console command; flash and mushroom-cloud visuals ride the effects channel with the fire path
- **entity**: The kill chain's damage half is closed (#626): every combat damage source funnels through one gate (`applyPointDamage`) where friendly fire finally means something; `DamageDef`'s thrust/control/avionics penalties act on the flight model instead of being parsed and ignored; hard ground impacts damage the airframe under the `crash_damage` toggle; the first production entity-event consumer turns kills into per-peer scores, a reliable kill-feed/stats channel (`MsgCombatEvent`, the last free ENet id, deliberately multiplexed), real debrief numbers, and a pilot-profile career log
- **content**: The missile endgame is decided (resolves RFC #676): kinematic flyout, ALL probability in the seeker (PoD dice through the shared detection math), proximity-fuze geometry at the end — no terminal Pk table. The weapon `[seeker]` block now references a sensor def (`sensor_id`) per the one-vocabulary rule; the ad-hoc `fov_deg`/`acquisition_nm` lobe is deprecated with one release of grace. New weapon fields: `mesh` (projectile visual), `pitbull_nm`, `loft_bias_deg`/`loft_range_nm`, `rate_of_fire_rpm`, `nuclear`/`yield_kt` (#676)
- **tools**: `validate-entity` — the entity-def validator that did not exist when a broken aircraft merged with all of CI green; `--pack` resolves every asset-name and def-id reference through the real content system, and an unresolvable `flight_model` is an error because the runtime fallback is silent (#829)

### Changed

- **tools**: `validate-weapon --pack` now validates the pack's weapons (per-file schema + plausibility, duplicate ids); the hardpoint↔weapon cross-check moved to `validate-entity --pack`, where the referencing files live (#829)

### Fixed

- **docs**: `ai.md` documented `pitch_error_from_alt` with the wrong signature and its own worked example used the wrong form — copy it and the AI errors every tick while flying straight ahead; the example is now executed as a test so the docs and the bindings cannot drift silently again (#830)

### Added

- **tools**: `fm-trim` gate rows can pin a Mach, a load factor and a payload — the Ps ladder is now checkable in CI (#826)
- **flight**: `[aero.controls] max_elevator_neg_deg` — asymmetric pitch travel (#822)

### Fixed

- **flight**: `fm-trim` declared every aircraft unflyable above ~8 km — the back side of the power curve (#825)
- **engine**: Hardened every TOML integer read against a float→int UB bug in toml++ (#824)

### Added

- **ui**: The in-game aircraft manual is generated from the flight model, not hand-written (#821)
- **tools**: `fm-trim` — derives an aircraft's performance from its flight model and gates it against the flight manual (#817)
- **flight**: `[aero.limits]` is enforced — stall flag, load factor, FBW G-limiter, over-G damage (#816)
- **flight**: `[aero.cd_table]` — tabulated total clean drag; the parabolic polar cannot represent a real fighter (#820)
- **content**: Weapons are a real asset type — `WeaponRegistry` + a default loadout that costs the airframe mass and drag (#812)

### Changed

- **flight**: `EntityDef` owns mesh/cockpit wiring; the dead `[aircraft] mesh`/`cockpit` keys are gone (#813)

### Fixed

- **flight**: The body-frame velocity update was missing the ω × v transport term, so a pitching aircraft developed no angle of attack and could not pull g (#816)
- **tools**: Flight-model plausibility bands excluded the entire light-fighter class (#815)
- **netcode**: The client flew a different flight model from the server, silently and permanently (#811)
- **content**: Def cross-references resolve by id, not filename — sensors reached the builtin eyeball on every pack aircraft (#810)

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
