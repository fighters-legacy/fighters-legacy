# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **ai**: `world.*` Lua module bindings for engine integration (Epic #584) — Lua AI/mission scripts gain a `world` module for spawning, faction relations, mission outcome, music, and event triggers: `world.spawn(type, pos, heading[, side])` → entity idx, `world.despawn(idx)`, `world.set_relationship(a, b, "friendly"|"neutral"|"hostile")`, `world.set_music_state("menu"|"patrol"|"combat"|"success"|"debrief")`, `world.mission_success()` / `world.mission_failure()`, `world.get_elapsed_time()`, `world.on_trigger(predicate_fn, callback_fn)` (fires once when the predicate first returns true), and `world.timer(seconds, callback_fn)` (one-shot). Because Lua runs on the sim thread with no sim→main queue, the engine-integration calls route through a host-injected `WorldApi` seam (`engine/script/WorldApi.h`) that fl-server wires to `EntityManager` / `FactionRegistry` / `MissionRuntime` and a new music broadcast; `on_trigger`/`timer` are pure Lua evaluated each tick. An unset hook is a safe no-op, so a controller with no host behaves exactly as before. A new reliable `MsgMusicState` (0x16) carries the transition to clients (which finally makes `GameState::FlightCombat` reachable), and `MissionRuntime::forceOutcome` lets a script drive the objective state machine. Closes #413
- **ai**: Lua coroutine-based AI control flow (Epic #584) — a script may now define `function ai_main()` and drive itself as a Lua coroutine resumed once per sim tick, writing sequential state machines with `coroutine.yield(control_table)` instead of the stateless `compute_control(state, tick, dt)` call model. The engine resumes `ai_main` with `(state, tick, dt)`; the coroutine yields a control table for the tick (the same fields as `compute_control` returns) and suspends until the next tick, where `yield`'s return values are the next `(state, tick, dt)`. A yield with no value is neutral for that tick; a finished or errored `ai_main` goes neutral forever (the same fail-safe as a `compute_control` error). The coroutine shares the sandbox's globals, so `guidance.*` / `nearby_entities` / `detected_contacts` and the deny-list all apply inside it. Purely additive: `ai_main` wins if both entry points are defined, and every existing `compute_control` script is unchanged. Closes #412
- **mission**: Weather trigger actions — `do: set_weather <preset>` and `do: set_time <hours>` (Epic #584) — mission designers can now script weather/time transitions from triggers (e.g. `on: destroy(sam1) / do: set_weather storm`). The objective evaluator already routed non-terminal `do:` actions through an injected dispatcher; fl-server now points that dispatcher at the **validated admin command path** (the same one the operator console and RCON use), so a mission can do exactly what an operator could and nothing more. `docs/modding/missions.md` documents the two actions and the routing model. Closes #212
- **mission**: Per-mission `time_scale` wiring via the mission loader (Epic #584) — the mission YAML `time_scale` field is now applied at runtime, not just validated. `WeatherController` gains `setTimeScaleRatio()` / `timeScaleRatio()` (previously the day/night rate was constructor-only), and `applyMission` applies the mission's `time_scale` alongside the weather preset / time-of-day / wind it already wired — so a mission that declares `time_scale: 20` runs its clock at 20× from tick 0, overriding the server-config default. A non-positive ratio is ignored (a frozen or reversed clock is a content bug), and a mission that omits the field leaves the host's configured rate untouched. Closes #207
- **test**: Multi-crew determinism, load, and CI gates (Epic #966) — the gates that keep multi-crew green as Phase 4 lands. A **two-own-records** broadcaster test proves the `isOwn` generalization: two humans on one crewed airframe (a pilot + a joined gunner) each receive that airframe as their own record (omega + loadout block). The **TSan serial-equivalence** leg (`tsan.yml`) now builds + runs `test_crew_frame` + `test_turret_gunner` under ThreadSanitizer, so the per-seat sample/slew/fire passes are byte-identical on 1 vs N workers with crewed entities present. A **crewed-AI load profile** — `[world] test_spawn_entity_type = "builtin:bomber"` (default = the single-seat debug entity) — points the scale-gate load AI at the crewed builtin bomber (a Fly pilot + a bot tail-gunner turret), exercising the per-seat passes + `SnapshotCrew` turret replication under 128-client load; the crew work is bounded (one bot gunner per airframe) so the committed `scale-gate-baseline.json` is expected to hold, and `docs/load-testing.md` documents the profile + how to justify any baseline movement. (Capability-partition, seat bind/vacate lifecycle, per-seat fire ordering, turret clamp/slew, and gunner fire were covered by the #968–#979 suites; this issue adds the missing two-own-records case, the TSan crew leg, and the load affordance.) Smoke-verified: fl-server spawns crewed bombers via the load path without error. Part of #966. Closes #980
- **game**: Gunner station — turret aim, slew prediction, gunner-keyed hit registration (Epic #966) — the client + server pieces that make a human turret gunner feel good under latency, following the #973 design. **Server:** the gun (hitscan) lag-compensation rewind (#425) is now keyed off the SHOOTING SEAT's occupant (`occupantPeerFor(airframe, seat)`) rather than the airframe's owning peer — a turret gunner's shot compensates by the GUNNER's latency, not the pilot's; the Fly seat's binding still resolves to the pilot, and the airframe-owner fallback keeps the single-seat case unchanged. **Client:** a pure `TurretPredictor` (`TurretPredictor.h`) predicts the gunner's own turret locally with the SAME `stepTurret` servo the server runs — exactly as `ClientPrediction` reuses `FlightIntegrator` — so the reticle tracks at frame rate; because a turret is a rate-limited scalar with no accumulating integration error, reconciliation to the replicated `SnapshotCrew` pose is a smooth blend (a large gap snaps), with no rubber-banding under normal RTT. The gunner's aim already reaches the turret via the seat-scoped `MsgClientInput::viewAxis` (#972), and `FireRequest` already carries the validated world bore (#970), so the shot resolves along the server-authoritative turret pose within its slew/arc limits — the client-favored path #973 recommended, with the client-asserted-bore escalation deferred behind its trigger. Tested: the predictor's rate-limited slew + blend/snap reconciliation + world-aim decomposition, and a human gunner's masked gun fire landing on a target through the gunner-keyed rewind path. (The lead-aware pipper reticle builds on the client `BallisticLead` + this predictor.) Part of #966. Closes #979
- **game**: Crew seat client — roster UI, seat selection, per-seat prediction gating (Epic #966) — the client surface for multi-crew. `ClientNetEventHandler` gains the seat protocol's client half: `sendSeatRequest` / `sendSeatLeave` (MsgSeatRequest) and `MsgSeatResult` parsing, plus `inCrewSeat()` (set from a granted non-fly join, since the server only ever grants non-fly seats). A pure, unit-tested `CrewSeatPicker` (`CrewSeatMenu.h`, the `EntitySelector` precedent) flattens every JOINABLE seat — a non-fly seat that is Empty or Bot-held; a human never displaces another human — across all known crewed aircraft into a deterministic cyclable list, and `occupiedSeat`/`seatIsFly` resolve which seat the player holds. A non-modal in-flight picker (`K` cycle, `L` join, `U` leave — the radio-menu pattern, axes stay live) drives it, surfacing the `MsgSeatResult` outcome as a one-line HUD message with client-side (localizable) strings. **Only the `Fly` seat runs flight prediction**: a joined gunner seat tears down `ClientPrediction` (like an observer) and views the host airframe without predicting its flight — the seat result is delivered before the re-setup ConnectAck so the gate is applied on the same frame. Documented in `docs/sandbox.md`. Tested: the seat-picker cycling/joinability/selection-preservation logic, and the client send + `MsgSeatResult` → `inCrewSeat` plumbing. (The graphical seat-selection menu and per-seat cockpit eyepoint camera build on this tested plumbing.) Part of #966. Closes #975
- **entity**: Crew seat damage — subsystem vocabulary and seat knockout (Epic #966) — a crew seat can be authored with `damage_hp` (an independent HP pool) + `hit_weight`, extending the #675 subsystem-damage model: a hit routes through the **same** weighted/quadrant pick — now over the fixed subsystems *and* the damageable seats combined, with a directional bias from `seat_pos` (a rear hit favors a rear seat) — so one hit damages one target. When a seat's pool is exhausted it is **knocked out** and goes silent, bot or human alike: a dead gunner's turret stops, a dead `fly` seat leaves the airframe uncontrolled (no pilot input reaches the integrator and its guns fall silent), and a human in a killed seat has its masked input ignored (its defined follow-on: stay a silent spectator of the airframe, or `leave` the seat to become an observer — distinct from airframe death). The knocked-out state replicates on the reliable crew roster (a `knockedOut` flag the client surfaces). Absent any `damage_hp` seat, behavior is exactly the pre-#978 fallback (the #675 model is untouched — its tests stay green). Deterministic + serial-equivalent (the `subsystemHash` integer-RNG idiom). Documented in `docs/modding/formats.md`. Tested: seat `damage_hp`/`hit_weight` parse + validation, the `crewSeatDamageBias` direction favoring the entry-side seat, a warhead knocking out a gunner seat (its fire falls silent), and a knocked-out `fly` seat silencing the pilot's guns. Part of #966. Closes #978
- **mission**: Mission crew configuration — skill ranges and per-seat overrides (Epic #966) — a mission object may carry a `crew:` block: an aircraft-level skill range `[min, max]` every bot seat rolls within, plus per-seat overrides (bot spec, skill fixed-or-range, and `empty` occupancy). It seeds the deterministic per-instance skill roll (#971) from the **mission name** so a flight of bombers is not uniformly deadly and a replay is byte-identical. `MissionParser` (the schema owner `validate-mission` delegates to) validates the block's shape (skill in `[0, 1]`, exactly one of `seat`/`role` per override); fl-server's mission `onSpawned` hook applies it via the new `WorldBroadcaster::applyCrewSpawnConfig`, which re-rolls each seat's bot with the effective skill range + mission seed and applies the `empty`/bot occupancy. The `WorldBroadcaster` seat-controller-factory seam now carries a `SeatBotContext` (skill range + mission seed) so the concrete gunner (in engine-ai) rolls the configured range while engine-net stays unlinked from engine-ai. `validate-mission --pack <dir>` cross-checks a `crew:` block against the referenced entity type's declared `[[crew]]` — a seat/role the entity does not declare, or a `crew:` block on a single-seat entity, is an error. Documented in `docs/modding/missions.md`. Tested: crew-block schema parsing + range/exactly-one validation, the `--pack` cross-check (valid seat/role accepted; bad role, out-of-range seat, and single-seat rejected), and an `empty` override silencing a bot seat's fire through a real broadcaster tick. Part of #966. Closes #976
- **server**: Seat join and handoff (Epic #966) — the full crew-seat lifecycle. `MsgSeatRequest` (ENet id `0x14`) / `MsgSeatResult` (`0x15`) let a human claim a non-fly seat or leave it in-session, and a `ConnectSeatClaim` TLV in the ConnectRequest `0x0500` range joins a seat at connect (falling back to a normal spawn when it is unavailable, so a pilot always gets in). **Free-form policy** — any peer may request any non-human-held seat, including hopping aircraft mid-flight; humans never displace humans (`SeatOccupiedByHuman`), and the Fly seat is not joinable via a seat request (it belongs to the owning pilot). A grant parks the seat's bot (dormant, its ammo/turret preserved), rebinds the peer, re-sends `MsgConnectAck` (the proven mid-session re-setup path) and the roster delta; vacate/disconnect reverts the seat to its authored default and the bot resumes mid-maneuver. The `despawnPeerEntity` decision tree is reworked so **the airframe is never destroyed out from under a remaining human**: a peer-spawned aircraft whose owning pilot leaves while a human gunner remains persists — its Fly-seat controller is swapped to a hold autopilot (no dangling `PeerController`), the Fly seat is vacated, and the orphaned airframe is retired only when its last human leaves; a single-occupant aircraft despawns on that pilot's disconnect exactly as before, and mission/AI airframes always persist. New operator commands `seats <entityIdx>` (inspect roster/occupancy) and `set_seat <entityIdx> <seat> <peerId|bot|empty>` (force a non-fly seat), documented in `docs/fl-server-config.md`. Fuzz seed added; `docs/network-protocol.md` tables updated. Tested (written first): join grants + binds, second-human denial, Fly-seat denial, pilot-disconnect-with-remaining-gunner persistence + last-human retirement, single-seat despawn parity, and the `seats`/`set_seat` surface. Part of #966. Closes #974
- **netcode**: Crew roster replication + seat-scoped client input (Epic #966) — the wire + server foundation for humans sharing one airframe. A new reliable `MsgCrewRoster` (ENet id `0x13`) carries a crewed aircraft's full seat roster — per seat the role, `CrewCapabilityMask`, turret index, per-instance skill, and three-state occupancy (`Empty` / `Bot` / `Human(peerId)`) — sent after `MsgFactionDef` for every crewed aircraft and re-broadcast on any occupancy change; a single-seat aircraft (the implicit-single-pilot fast path) sends none. The peer binding generalizes from `peer → aircraft` to `peer → {aircraft, seat}` (`m_peerSeat`, `occupantPeerFor`), so the snapshot **own-record** (omega + loadout) now goes to *every* seat occupant, not just the airframe owner, and each crewed aircraft's live **turret pose** (mount-frame az/el, quantized to `int16`) replicates to interested viewers via a new interest-filtered `SnapshotCrew` snapshot TLV (`0x0106`) — **absent for single-seat aircraft, so their snapshots stay byte-identical**. A seat occupant's unmodified 80-byte `MsgClientInput` is **masked server-side by the seat's capabilities** (`engine/net/SeatInput.h`): a non-Fly seat never drives flight (never trusted-zeroed — masked), a Fire/turret seat's `viewAxis` becomes the commanded turret direction, and `selectedStation` clamps to the seat's own station partition. `setSeatOccupant`/`clearSeatOccupant` are the binding mechanism the seat-join protocol (#974) drives; a gunner's disconnect reverts its seat to the authored bot. The client (`ClientNetEventHandler`) parses both the roster and the turret-pose TLV into a queryable `CrewClientState` for the seat UI (#975) and gunner station (#979). Fuzz seed added for the new codecs; `docs/network-protocol.md` tables updated. Tested: wire round-trip, capability-mask routing, a human gunner firing along its own aim through a real broadcaster tick, seat vacate resuming the bot, roster-on-connect, and the single-seat-byte-identical guarantee. Part of #966. Closes #972
- **docs**: Client-favored turret hit-registration design (Epic #966) — a spike/design record (`docs/turret-hit-registration.md`) resolving how a human turret gunner's shots register fairly under latency without trusting the client for kills. Grounds the design in the existing code: the turret slew is already server-authoritative (`stepTurret`/`commandTurretWorld`) and `resolveHitscan` already does bounded lag-compensation rewind (#425) keyed off the *airframe* peer — so the actionable core for #979 is to key that rewind off the **gunner seat's occupant** delay, add a client turret-pose predictor that reuses `stepTurret` verbatim (mirroring `ClientPrediction`'s reuse of `FlightIntegrator`), and reconcile it against the `SnapshotCrew` TLV. Recommends implementing that spine in #979 and **defers** the client-asserted-bore-with-`stepTurret`-replay-validation escalation behind a measured trigger, with a server-authoritative-slew-only fallback. Part of #966. Closes #973

- **content**: Builtin multi-crew bomber — zero-pack crew/turret provability (Epic #966) — a compiled-in `builtin:bomber` EntityDef: a Fly+Fire+Radar+Countermeasures pilot dropping bombs off station 0, plus a defensive **tail-gunner** seat aiming a rear-facing cannon turret (station 1) filled by the builtin turret gunner. The crewed counterpart to `builtin:debug-entity` — it makes the whole crew → per-seat sampling → turret-slew → directional-fire path provable with **zero content packs**, per the armed-sandbox doctrine, and it is added to the `builtin:sandbox` mission (Instant Action) as a red bomber whose bot gunner auto-defends its rear quarter. Registered by both fl-server and the game client (with the client-side def rebuild so the manual can render it). The generated in-game aircraft manual (#821) gains a **Crew** section for a crewed aircraft — seat roles, capabilities, and turret arcs, all generated from the def (a single-seat aircraft gets no such section). Tested: the bomber is a valid crewed partition with a resolvable tail gun; the sandbox parses with the bomber object; the manual renders the crew page. Closes #977. Part of #966
- **ai**: Bot turret gunner + per-instance skill (Epic #966) — a defensive turret seat now fills by default with a `TurretGunnerController` (`ISeatController`) that acquires **honestly** off the airframe's shared contact table (a jammed/blind aircraft's gunner engages nothing), leads the target with the shared `BallisticLead` solver, commands the turret onto the lead (the server servo clamps + slews it within the traverse limits), and holds fire until the gun is actually pointed at the target inside the lethal cone — firing along the turret bore, not the airframe nose. It carries the **first per-instance skill in the engine** (`engine/ai/PerInstanceSkill.h`, the `detectionHash` integer-RNG idiom, seeded by mission seed ⊕ object id ⊕ seat index so a flight is not uniformly skilled and a replay is byte-identical): a higher rolled skill measurably tightens the aim (a smaller error cone — the **first consumer of `AiScaling::aimErrorDeg`**) and shortens the reaction before opening fire. Authored with `bot = "gunner"` / `"builtin:gunner"` (or a Fire seat aiming a turret with an empty spec); fl-server injects it via `setSeatControllerFactory` (engine-net does not link engine-ai — the std::function seam). Per-instance skill is generalized so plain mission AI can use it too. Documented in `docs/modding/ai.md`; tested (deterministic roll; a bot gunner acquires + fires on a hostile it honestly detects; a higher skill fires sooner and hits harder; TSan-clean). Closes #971. Part of #966
- **server**: Crewed control frame — per-seat sampling and capability-masked merge (Epic #966) — a `WorldBroadcaster` now steps a multi-seat aircraft as a per-seat control frame inside the existing data-parallel tick. The Fly seat flies via its `IEntityController` (today's pilot path); each non-fly bot seat is an `ISeatController` (`SeatCommand sample(...)`, deliberately narrower — a gunner does not fly) sampled in the AI pass, which commands its turret; the integrate pass slews turrets via the pure `stepTurret` servo; the serial weapons pass evaluates each seat's fire over its own **disjoint loadout partition** and fires a turret seat's stores along the turret bore. The one-owner-per-channel invariant makes each seat's ammo exclusively its own, so no shared-mutable loadout or `FireState` split was needed; `WeaponControls` (which both `ControlInput` and `SeatCommand` project onto) is the fire slice `evaluateFire` reads, and `FireRequest` gained a `seat` field so the deterministic sort key is `(shooterIdx, seat, station)`. **A plain single-seat fighter is byte-for-byte the previous path** (`CrewState` empty → the existing controller/fire code runs unchanged; all prior broadcaster/fire tests stay green). `setSeatControllerFactory` injects the seat bots (the gunner is a follow-on); bots-only in this wave (no humans, no wire yet). New Catch2 coverage: a bot gunner fires along the turret bore not the airframe nose, an empty seat contributes no fire, and the per-seat pass is serial-equivalent + TSan-clean across 1 vs N sim workers. Closes #969. Part of #966
- **flight**: Turret mounts — traverse limits, server slew, directional launch (Epic #966) — a weapon station can now aim independently of the airframe nose. New pure `engine/weapon/Turret.h`: a `TurretState` slewed by a rate-limited, limit-clamped `stepTurret` servo (server-authoritative — no instant aim regardless of the client; the same pure function is the client turret predictor), plus the mount-frame pose math (`turretBoreMount`/`turretAimToAzEl`/`turretWorldDir`) and the world→mount command path. `FireRequest` gains an optional world-space launch direction; `ProjectileSystem::launch` and the `WorldBroadcaster` hitscan path fire along it when present, else along the airframe nose exactly as before (nose fire is bit-identical). This **is** the ground-SAM launcher-elevation gap documented as owed to #585: a static emplacement gets elevation by mounting its launcher as a turret and slewing it via this servo (the SAM/AAA controller wiring is the crewed-control follow-on). Unit-tested for clamp/slew/shortest-arc edge cases and an end-to-end store leaving along the turret bore; documented in `docs/modding/weapons-sensors.md`. Closes #970. Part of #966
- **entity**: Crew seat + turret mount schema (Epic #966) — an `EntityDef` gains optional `[[crew]]` seats and `[[turrets]]` mounts. A seat's `role` is a display string; the engine sees a `CrewCapabilityMask` (`fly`/`fire`/`radar`/`countermeasures`/`command`) — roles-as-data, per #944. **Absent `[[crew]]` is an implicit single pilot**, so every existing aircraft is a valid one-seat crewed aircraft with zero content changes and the single-seat path is unchanged. A seat may bind hardpoint stations directly or via a turret it aims, may spawn a bot (`bot = "gunner"` / `"builtin:gunner"` / `"lua:<script>"`) or empty (`empty = true`), and carries a per-instance skill baseline. A `[[turrets]]` mount gives a weapon station az/el traverse limits, a server slew rate, and a body-frame rest orientation (the runtime slew servo + directional launch land in the turret-mounts issue). `parseEntityDef` (shared with `validate-entity`) enforces the **one-owner-per-channel** invariant — exactly one `fly` seat, and each hardpoint/turret/`radar`/`countermeasures`/`command` owned by at most one seat — so a capability-masked merge of crew inputs can never conflict; `validate-entity --pack` additionally resolves a `lua:` bot spec to a pack AI script. Schema + validation only — no runtime behavior yet. Documented in `docs/modding/formats.md`. Closes #968. Part of #966
- **docs**: Multi-crew aircraft decision record + roadmap entry (Epic #966) — a dated decision record in `docs/architecture.md#decision-records` captures the multi-crew architecture before implementation: crew seats unified into `ControlledEntity` as a `CrewState` (not child entities, not a controller facade — both alternatives pressure-tested and rejected); the `CrewCapabilityMask` capability partition (roles-as-data, per #944) with the one-owner-per-channel invariant; server-authoritative turrets with a directional launch vector (delivering the ground-SAM launcher-elevation gap owed to #585); the additive wire surface (`MsgCrewRoster`/`MsgSeatRequest`/`MsgSeatResult`, ConnectAck seat field, seat-scoped `MsgClientInput` routing, snapshot crew block, `kProtocolVersion` unchanged); three-state `{Bot | Human | Empty}` occupancy; and the first per-instance skill (finally a consumer for `AiScaling::aimErrorDeg`). Adds the epic to `docs/roadmap.md` as a Phase 4 acceptance row and notes in `docs/ai-architecture.md` that crew seats (the `Command` capability + roster) are the embodiment surface Epic O / #591 later binds to. Closes #967. Part of #966
- **render**: HD satellite terrain textures (Sentinel-2) — terrain tiles can now carry real cloud-free orthophoto imagery instead of the procedural biomes. A new tool `tools/gen_terrain_color.py` discovers cloud-free Sentinel-2 L2A scenes via the earth-search STAC API (AWS Open Data, anonymous — no account), reads the RGB + scene-classification bands through GDAL `/vsicurl`, builds a cloud-masked median composite, converts reflectance to sRGB, and BC7-encodes each cube-sphere tile to `tile_<i>_<j>_sat.ktx2` (via `tex-compress`, mipmapped). The `TerrainStreamer` loads the `_sat.ktx2` layer (client-only — a headless server skips it), creates a per-tile material, and the terrain shader samples the imagery (`kRenderFlagTerrainSatellite`, `mesh.frag` shadingMode 4) — and it drapes over PROCEDURAL terrain too, not only pack DEM tiles. The Vulkan material pool grew to 2048 for the per-tile materials. Copernicus Sentinel data terms require the "Contains modified Copernicus Sentinel data" attribution when derived tiles ship (see NOTICE / `docs/modding/satellite-terrain.md`). The tool's pure logic (cloud masking, sRGB conversion, scene selection, tile coverage) is unit-tested; the render path is unit-tested (texture/material/flag) and GPU-verified against a real San Francisco Bay composite. Closes #488. Part of #468
- **terrain**: GEBCO bathymetry in the global base bundle — `tools/build_global_base.sh` gains a `--bathymetry` flag that forwards a GEBCO grid to the tile generator's existing bathymetry merge, so the bundled coarse base terrain carries real sea-floor depth (shelves, basins, trenches) instead of flat ocean at the datum. Two documented recipes: GEBCO alone as `--elevation` (land + ocean), or a land DEM plus `--bathymetry` GEBCO merged underneath. The engine already queries ocean depth (`oceanDepthAt`/`isShallowWater`); this feeds it real data. NOTICE gains the GEBCO_2024 citation (DOI). The base-terrain bundle is produced out-of-band from the ~7.5 GB GEBCO grid and shipped with releases, not committed. Closes #476. Part of #468
- **content**: Per-theater altitude wind profiles — wind can now vary with altitude instead of a single ground-level vector, so a high-flying aircraft feels a different wind than one on the deck (and, once the projectile seam lands, so will ordnance). A profile is a handful of knots (altitude, speed, heading-FROM) that both the server and the client interpolate with the SAME shared code (`engine/weather/WindProfile.h`), so client prediction stays in parity by construction. The server loads a profile from `[wind] profile_path` in `server.toml`; it is broadcast to clients as an additive TLV on `MsgWeatherState` (old clients ignore it and keep the datum-level wind — no version bump). A new source-agnostic tool `tools/gen_wind_profile.py` derives a profile from a gridded NetCDF wind dataset (a NASA MERRA-2 file with an EarthData account, OR an anonymous NOAA GFS file converted with `wgrib2` — variable names are flags, the tool never fetches), taking a seasonal median over a bounding box. Unit-tested: the interpolation and clamping, the WeatherController authoring (heading→vector, sort, gust fold), the config + profile-TOML parse, and the tool's pure logic (cardinal-wind heading convention, ISA pressure→altitude, antimeridian bbox). Closes #489. Part of #468
- **platform**: HTTP(S) content-download stack — a streaming `IHttpClient` HAL (single background worker, results drained on the main thread via `service()`, mirroring `IAsyncFilesystem`) with a libcurl backend (`platform-http`), a clean-room streaming **SHA-256** (`engine-crypto`, FIPS 180-4, stdlib-only — deliberately not OpenSSL, which is GNS-gated), and an engine-side `ContentDownloader` that streams a manifest of files into the user data dir, hashes each incrementally, and only renames a `.part` into place when the SHA-256 matches — so a corrupt or truncated transfer never lands as a real file. The engine stays HAL-clean (ContentDownloader takes `IHttpClient` by injection; the module-boundary guard forbids any `engine-*` target from linking the backend). TLS validation is always on and https→http redirect downgrades are always refused. The libcurl backend builds where a system libcurl is present (`find_package(CURL)`; CI installs `libcurl4-openssl-dev`) and cleanly disables to a nullptr client otherwise. SHA-256 is verified against the NIST vectors and the downloader end-to-end against a canned client (good file installs, hash mismatch / HTTP error reject and install nothing). Closes #490. Part of #468
- **renderer**: Geographically-oriented night sky — stars and the Moon now render in the sky pass. The star field is procedural but its orientation is real: rays are looked up in a celestial (equatorial) frame built from Greenwich sidereal time and the observer's latitude/longitude (`CelestialFrame.h`), so the whole field turns about the pole through the night and the celestial pole sits at an altitude equal to the latitude. The Moon is placed at its true position from a truncated Meeus ephemeris (`LunarPosition.h`) and rendered as a lit disc whose phase falls out of the Moon/Sun geometry in-shader (the lit limb faces the Sun). The sky darkens toward night as the Sun sets, and both fade with cloud cover. Computed client-side per observer beside the geographic Sun (#481), so no wire change. The celestial math is unit-tested against known epochs (GMST at J2000, pole altitude, zenith culmination, new/full-Moon illumination); GPU-verified. Closes #484. Part of #468
- **terrain**: Land-cover-driven biome shading with spherical-valid detail coordinates — terrain tiles carry an ESA WorldCover class per vertex, so biomes are chosen from real land-cover data (grass/dirt/rock/snow, with open water rendered dark + glossy and steep ground forced to rock) instead of guessing from elevation and slope; a tile with no `_lc` layer falls back to the old elevation/slope selection. The per-vertex data (class, normalized elevation, and a **spherical-valid detail coordinate**) is packed into the mesh's TANGENT attribute — which terrain does not otherwise use — so no new vertex layout or pipeline was needed. The detail coordinate is the global cube-face-UV arc length reduced modulo a common multiple of the fine/coarse tiling, so ground-detail tiling is now seamless across tile borders and LOD levels (it no longer derives from `worldPos + worldOrigin`, which cancelled to metre-precision garbage at Earth radius); slope is measured against the true radial "up" via a camera-relative planet centre added to the camera UBO. The biome-weight table (`engine/render/BiomeWeights.h`) is the single source of truth, mirrored in the terrain shader and pinned by a test. Closes #475. Part of #468
- **renderer**: Content-pack biome texture arrays for terrain — the terrain forward path now samples a 2D texture array of per-biome PBR maps (base color + combined normal/roughness) instead of flat procedural tints, blending a fine (~4 m) and a coarse (~30 m) detail scale, so ground gains real surface grain up close. A content pack supplies `textures/biome_basecolor.ktx2` + `textures/biome_normalorm.ktx2` (the array layer index is the biome id: 0 grass, 1 dirt, 2 rock, 3 snow — see `docs/modding/textures.md`); with no pack, deterministic byte-stable builtin biome arrays (`BuiltinBiomes`) render zero-pack. The arrays bind through a new descriptor set (`IRenderer::createTextureArray` + `setTerrainBiomeTextures`; `TerrainStreamer` uploads them once at construction, skipped headlessly on the server), and `VkResources` gains genuine 2D-array upload (KTX2 layers or the raw-RGBA path). Also lands a **serialized pipeline cache** rider — the Vulkan pipeline cache is loaded from and saved to the user pref dir, trimming shader pipeline build time on repeat launches. Closes #446. Part of #468
- **tools**: `tex-compress` 2D-array KTX2 output — a new `--layers <in…>` mode packs N layer-major PNGs into a single 2D-array KTX2 (`toktx --layers`), the format the terrain biome arrays consume, with the array index as the biome id (0 grass, 1 dirt, 2 rock, 3 snow). Adds `-o/--output` (required in array mode). Documented in `docs/modding/textures.md` with the load-bearing layer-order convention; unit-tested (command string + layer order + the ≥2-layer guard) and a CI step that installs `toktx` and runs a real 4-layer encode, asserting `layerCount == 4` (the gate actually encodes, not just `--version`). Closes #447. Part of #468
- **render**: Procedural runway rendering + runway surface typing + surface-dependent ground physics — a new `AirportRenderer` draws each nearby runway as a camera-relative slab tessellated along the spherical datum a few cm above the flattened pad (a flat quad would chord below the curved surface), tinted per surface and distance-culled via the airport grid; paved runways carry procedural markings (centerline dashes, edge lines, threshold bars) generated in-shader from the slab's runway-local UV (`mesh.frag` shadingMode 3, no textures). `TerrainStreamer::surfaceTypeAt` now reports the runway surface inside a footprint via an injected override (the terrain `SurfaceType` grows Concrete/Asphalt/Gravel/Deck; the `RunwaySurface` authoring enum bridges to it through `RunwaySurfaceMap.h`), and that surface drives **ground physics**: `FlightIntegrator::step` applies per-surface rolling resistance (`GroundSurface.h` `GroundFriction`), so a rollout on grass is shorter than on concrete — wired identically on the server (`WorldBroadcaster::setGroundSurfaceQuery`) and the client (`ClientPrediction` surface query) via one shared `groundFrictionFor` table, keeping prediction in parity. Closes #487. Part of #468
- **render**: In-engine frame screenshot capture — `IRenderer::captureScreenshot(path)` (Vulkan backend reads the presented swapchain image back to a PNG) + a `--screenshot <path> [--screenshot-frames N]` game flag that writes one frame after the session streams in, then exits. The reliable in-engine path for visual verification (no external screenshot tool); groundwork for the demo-video capture pipeline. Part of #909
- **world**: Real-world airports + runway terrain flattening — the bundled public-domain OurAirports database (`data/airports.csv` + `data/runways.csv`, ~72k open airports, REUSE-annotated) is imported on first launch into a deterministic little-endian `cache/airports.flab` binary index (versioned + FNV-1a source-hashed, byte-identical across platforms; re-imported only when the CSVs change) and merged with the builtin airfield + pack airports into the `AirportRegistry`. The registry gains a 1-degree lat/lon bucket grid (`airportsNear`/`nearestTo`, pole- and antimeridian-correct) and **runway terrain flattening**: `AirportRegistry::flattenedHeight` grades the terrain to the authoritative field elevation across a runway footprint (flat core + smoothstep blend), wired into a new `TerrainStreamer::setHeightModifier` seam that is applied to BOTH the `heightAt` physics-floor query AND the tile-mesh vertices — so gear touches at the field elevation and the rendered terrain, the server floor, and the client's predicted floor all agree by construction (the client loads the identical registry from the same bundled data). fl-server and the game client both wire it before the sim starts. **Also fixed a release-packaging bug found here**: `data/` was never installed, so every release shipped without the airport database (and the WMM.COF reference copy) — a `COMPONENT runtime` install rule now ships it. Closes #486. Part of #468
- **world**: Airport and runway definitions — new `engine/world/AirportDef.h` (`AirportDef`/`RunwayDef` + a `RunwaySurface` authoring enum, deliberately distinct from the WorldCover land-cover `SurfaceType`) and a load-once, lock-free `AirportRegistry` (the `FactionRegistry` pattern) that resolves each field's world position and per-runway geometry on the sphere via `geodeticToWorld`/`enuBasis`, taking terrain-resolved field elevations from an injected height function. Airports are a content-pack asset (`airports/<name>.toml` → `AssetType::Airport`, wired end to end through `IContentPack`/`AssetManager`/`ContentBootstrap::registerPackAirportDefs`), placed EITHER geodetically (lat/lon) OR in world-XZ (near the origin/pole where lat/lon is singular). Ships a compiled-in `builtin:airfield` — a 2500×45 m asphalt strip a few km east of the sandbox spawn — so a runway exists zero-pack, and adds an `EntityDef::accepts_landings` carrier/flight-deck seam (data-only until #38). fl-server loads the registry (builtin + pack airports) before the sim starts. Closes #699. Part of #468

## [0.3.7] - 2026-07-17

### Added

- **flight**: Afterburner envelope limits — new optional `[engine] ab_min_mach` and `ab_max_alt_km` on the flight model gate `FlightState::ab_engaged` in `FlightIntegrator::step`, so the augmentor no longer lights at 0 kt on the runway or at 30 km. AB extinguishes below the ram-limit Mach or above the altitude ceiling even with the throttle in zone, with a small hysteresis band so a model riding the boundary does not chatter. Both limits are optional with permissive defaults (a model omitting them is bit-identical to before); parsed via the runtime `parseFlightModel`, range-checked by `validate-flight-model` (which also warns when set without an `ab_thrust` deck), documented in `docs/modding/formats.md`. Closes #309. Part of #585
- **flight**: Engine-out asymmetry parameterized by engine count — new optional `[engine] engine_count` (default 2) and `engine_yaw_arm_frac` (default 0.15) on the flight model. Losing one engine now removes `1/engine_count` of thrust (a four-engine airframe sheds a quarter, not the previously hardcoded half that silently assumed a twin) and yaws toward the dead side with a wingspan-fraction moment arm. Both default to bit-identical previous behavior when omitted; parsed via the runtime `parseFlightModel`, range-checked by `validate-flight-model`, documented in `docs/modding/formats.md`. Part of #585 (#308)
- **engine**: Sensor framework — radar operating modes, IRST/RWR (Epic F). The sensor core (#677) grows player/AI-facing behavior: a per-observer `sensor::RadarMode` (Silent/Search/TWS/STT) governs radar-typed sensors — Silent is true EMCON (a radar now sees **nothing** it does not radiate, closing the pre-#526 passive-radar-search free lunch), Search reports a bearing but never a lock, TWS scans and holds soft (non-firing-quality) tracks, and STT dedicates the radar to one designated target with a firing-quality lock (`Contact::firingQuality`). A serial **RWR** inversion pass (`SensorSystem::buildThreatWarnings`) turns every emitter's contacts into its targets' radar-warning picture (`ThreatWarning`/`ThreatWarningSet`: strobe vs lock tone, emitter bearing) — honest by construction, a receiver hears only a beam actually on it. Radar mode is player-controllable over the wire (`MsgClientInput::radarMode`, an absolute field in existing reserved space — no size change, no version bump) and reaches AI/evade controllers through the new `AiTickContext::threats` seam; `WorldBroadcaster::setRadarMode`/`setDesignatedTarget`/`threatsFor` expose it to missions/admin (#526)
- **engine**: IFF / identification (Epic F). Every `sensor::Contact` now carries an honest `Identification` (Friend / Foe / Unknown) computed each check — NOT the target's raw faction, which would be an identification wallhack. A friend squawks (a friendly coalition relationship ⇒ Friend at any range); a hostile is Unknown until positively identified (a visual contact or a committed firing-quality STT lock ⇒ Foe), so a distant enemy is an ambiguous blip until you merge or lock it — the ROE commit loop BVR combat turns on. The pure `classifyIff` + `affiliationRelation` live in `engine/sensor/Iff.h`; the observer→target coalition relationship is injected via `SensorSystem::setIffResolver` (a `std::function`, so engine-sensor keeps no engine-world link), wired in `WorldBroadcaster` to the `FactionRegistry` relationship matrix with the `fl::hostile()` affiliation fallback before a mission loads (#527)
- **network**: Datalink / shared team track picture (Epic F). A new `MsgDatalink` (`MsgId 0x12`, server→client, unreliable, ~6 Hz per peer) delivers each pilot the **fused** picture of everything their team can see: the server merges the peer's own sensor contacts with every same-faction teammate's, deduplicated by target (`sensor::TrackFuser`, a pure order-independent merge), so you see the bandit your wingman locked even if your own radar never found it. Each track carries its `Identification` (the display-safe IFF fact, not the raw faction), a firing-quality flag, and an own-sensor-vs-datalink-only flag; RWR strobes ride the same message. The client (`ClientNetEventHandler::radarView`) reconstructs absolute positions from the header origin and the HUD draws a 360° PPI radar scope + RWR ring in Cockpit mode, coloured green/red/amber by IFF. Radar mode is now player-cyclable with **R** (Silent→Search→TWS→STT). New `engine/render/RadarView.h` view types + `engine/sensor/TrackPicture.h` fuser (#528)
- **engine**: ECM/ECCM and chaff/flare expendables (Epic F) — wiring the countermeasure seams the codebase had scaffolded but never connected. New `engine/weapon/CountermeasureSystem.h/.cpp`: per-entity chaff/flare magazines (`EntityDef::chaffCount`/`flareCount`) + a pool of dispensed decoys that lag the aircraft, and a deterministic `seduces()` verdict a missile seeker asks each check. The `ProjectileSystem` countermeasure seam (dormant since it was landed) is now wired end-to-end: a flare seduces an IR seeker, chaff a radar one — gated by the missile's per-head `WeaponDef::countermeasures` susceptibility (finally consumed) — breaking the lock for both self-guided and SARH/pre-pitbull shots. **ECM/ECCM**: a jamming target (`EntityState::ecmActive`) denies a hostile radar a *lock* beyond a burn-through range (a bearing strobe is all it gets until it closes in); a radar's new `SensorDef::eccm` extends that burn-through. Players dispense with **E** and toggle the jammer with **J** (`MsgClientInput` buttons bits 3/4 + `ControlInput::dispenseCm`/`ecm`); a `CountermeasureRelease` cosmetic effect pops on the client. Tested: seduction (channel, susceptibility, proximity, expiry, determinism) + burn-through/ECCM (#529)
- **docs**: `docs/modding/weapons-sensors.md` — the combat-systems authoring guide (Epic F). Ties together the one-vocabulary sensor model, `[signatures]` (the target side), sensor defs + radar modes/RWR, IFF/identification, the datalink shared track picture, weapon seekers, and EW (chaff/flare susceptibility, countermeasure magazines, ECM/ECCM burn-through) with worked examples. Cross-linked from the README index and `formats.md` (which stays the field-level reference) (#530)
- **audio**: Cockpit warning tones — new `engine/audio/WarningToneManager` drives stall and overspeed audio cues from the own aircraft's predicted `FlightState` (stall from `FlightState::stalled`, overspeed from Mach beyond `[aero.limits] max_mach`). A small, headless-unit-tested state machine with entry/exit hysteresis so a threshold flicker never chatters the tone; head-locked looping sources with byte-stable compiled-in tones (an intermittent stall horn, a steady overspeed clacker) so it sounds zero-pack; gain follows the master·SFX volume live. `ClientPrediction::predictedState()`/`predictedMaxMach()` expose the own-ship state the snapshot does not carry. Null audio device tolerated (CI opens none). RWR/missile-lock tones are tracked separately on #960 (Epic F dependency). Part of #586 (#957)
- **terrain**: Ocean bathymetry support — `TerrainStreamer::oceanDepthAt(dvec3)` (depth below the sphere datum, 0 over land) + `isShallowWater` deep/shallow classification, and a `--bathymetry-source` option in `gen_terrain_tiles.py` that fills a land DEM's ocean/nodata gaps with GEBCO sea-floor depth (`merge_bathymetry`, pure numpy, pytest-covered) so ocean tiles encode real negative elevation instead of flattening to sea level. Part of #476 (bundling actual GEBCO data is a follow-on)
- **terrain**: Typed terrain surface classification — new `engine/render/SurfaceType.h` (`Unknown/Water/Grass/Forest/Urban/Snow/Rock/Wetland`) + `surfaceTypeFromWorldCover` mapping the ESA WorldCover class codes, and `TerrainStreamer::surfaceTypeAt(dvec3)` so gameplay/physics query a named surface instead of a raw class byte. The land-cover tile plumbing (layer load, `surfaceAt`, COLOR_0 emit) already exists; the class-driven biome **shader** rendering remains (it needs GPU visual verification). Part of #475
- **docs**: `NOTICE` file recording third-party data-source attribution and licenses — the bundled public-domain WMM2025 coefficients (#483), and the attribution obligations (ESA WorldCover CC BY 4.0, Copernicus Sentinel-2, GEBCO, OurAirports) recorded ahead of bundling so the requirement is in place before that data ships. Linked from the README documentation index; REUSE/SPDX metadata covers every bundled data file. The player-reachable in-game credits surface lands together with the first attribution-bearing dataset it must display (the current bundled data is public-domain and imposes no such requirement) (#485)
- **engine**: World Magnetic Model (WMM2025) — new `engine/nav/MagneticModel.{h,cpp}` implements the degree-and-order-12 spherical-harmonic geomagnetic synthesis (declination, inclination, field components) with the WGS84 geodetic→geocentric conversion, fed by the bundled public-domain NOAA/NGA WMM2025 Gauss coefficients (compiled-in; `data/WMM.COF` ships the human-readable copy). Validated against NOAA's official WMM2025 test values (declination at all 9 reference points + H/F/I). The flight HUD now shows a magnetic heading (`MAG`) alongside true `HDG` — magnetic = true − declination. `engine-nav` is a new stdlib-only library; `data/WMM.COF` carries a REUSE public-domain annotation (#483)
- **engine/renderer**: Geographic solar position — new `engine/weather/SolarPosition.h` (NOAA solar-position algorithm: declination, equation of time, azimuth/elevation from latitude/longitude + UTC Julian Day). `WeatherController` gains a UTC date/clock (`utcJulianDay`, `setDate`) that advances across midnight, broadcast in `MsgWeatherState` (grew 24→32 bytes, additive `utcJulianDay` double, no version bump). The sun is now computed **per-observer on the client** each frame from the camera's own latitude/longitude, so the day/night terminator moves correctly across longitudes and two players far apart see different local suns; the legacy planar sun remains the server/nominal fallback. Feeds the existing `SkyUBO.sunDirection`, so no sky-shader change was needed (#481)
- **flight**: Earth-fixed rotating world frame — `FlightIntegrator` adds the Coriolis (`−2ω×v`) and centrifugal (`−ω×(ω×r)`) accelerations, spin axis along world +Y at Ω = 7.292×10⁻⁵ rad/s (`kEarthRotationRate`), so long-range gunnery/ballistics and navigation deflect correctly. Per-instance `setEarthRotationRate` defaults to 0 (inertial frame) so every existing near-origin test is bit-identical; fl-server enables it from `[world] earth_rotation` (default `true`) and client-side prediction models the same deterministic terms, keeping reconciliation in exact parity. Both terms depend only on the axis-perpendicular `(x, z)`, so they vanish exactly at the world origin/north pole (#482)
- **flight**: Airspeed distinctions in `engine/flight/Atmosphere.h` — `machNumber`, `dynamicPressurePa`, `equivalentAirspeed`, `calibratedAirspeed` (compressible impact pressure, subsonic + supersonic Rayleigh branch), and `pressureAltitudeM` (ISA inversion). The flight HUD's "IAS" now reads true calibrated airspeed (was raw world-velocity magnitude mislabeled "IAS", reading a whole density-lapse high at altitude) and adds a Mach readout; the ~4 duplicate inline `spd / speed_of_sound` Mach sites in the integrator and trim now call the shared `machNumber` (bit-identical). Part of the spherical-Earth epic; the USSA-76 density model to 86 km already landed via #354 (#480)

### Changed

- **docs**: Holistic program review (2026-07-17) — roadmap catch-up (Phase 3 marked complete/gated at v0.3.0; Epic L GNS validation #649/#653 done; critical path rewritten to the five live chains toward v0.4.0; new "Deferred Levers" registry for the trigger-gated #575/#572/#652 levers); Phase 4 marked the active milestone; new decision records (VR/OpenXR deferred to Phase 8, not rejected; the 2026-07-01 transport record annotated for the OpenSSL/system-protobuf reversal); MCP reframed as a first-class operator/modding surface; the deterministic CPU-viable STT voice-command tier and Piper TTS noted as outside the 9B/GPU floor; a player-facing netcode-trust preamble; accessibility folded into the "approachable" design pillar; voice/AI-comms prior art; project-management conventions (decompose-at-phase-entry, Effort field retired, permanent Order bands, cross-repo triage parenting)
- **docs**: Release-note prose convention + theme-first naming — `docs/project-management.md`'s "Cutting a release" gains an explicit step to **hand-author the GitHub Release body as prose** (a one-to-two paragraph thematic summary + the per-issue CHANGELOG detail), because git-cliff collapses a squash-merged multi-issue PR to a single line; and milestones/epics are named by a theme phrase rather than a bare number/letter so ordering never reads as priority (#954)

## [0.3.6] - 2026-07-17

### Added

- **docs**: Authoritative "Content Pack Linking Exception" text in `GOVERNANCE.md` — defines the Vendorable Interface Set (`IContentPack.h`, `AssetTypes.h`, `TrustLevel.h`), what "link against" permits, and what remains GPL; `IContentPack.h`'s header pointer now resolves (#663)
- **sensor**: Optional `[sensor] role` (`aircraft`/`seeker`, default `aircraft`); tooling-only. A `seeker` head is exempt from the non-emitting-track-lobe warning (#902)
- **ai**: `DynamicLoiterController` orbits a **moving** entity — re-centers the loiter circle on the target's live position each tick and matches its altitude; exposed as the `dynamic_loiter` AI behavior, and the `escort` template now tracks the moving escortee instead of its spawn point (#464)
- **engine**: `IWindow::showFolderDialog(title, defaultLocation)` — native folder picker for content-pack `configure()` UIs; non-pure with a nullopt default (implementors/mocks keep compiling), SDL3 backend via `SDL_ShowFileDialogWithProperties`. Adds a virtual to the `IWindow` vtable — rebuild native plugins against this header revision (#665)
- **tools**: `bot_swarm` samples server RSS periodically into an `rss_series` (report `schema_version` 4) and gates on the growth **trend** — `--assert-max-rss-slope-kb-per-min` fits the run's tail and fails a slow leak that stayed under the endpoint bound; wired into the `soak` scale-gate profile (#789)
- **network**: `SnapshotLastAckedSeqNum` snapshot TLV (`0x0105`) carries the exact `seqNum` the server last applied for a peer, so client-side prediction replays precisely the un-reflected inputs instead of approximating the window from `estimatedDelayTicks` — exact under high delay variance; additive, falls back to the delay-ticks estimate when absent (#427)
- **flight**: `MsgWeatherState` broadcasts the turbulence amplitude (grew 20→24 bytes, additive) so client-side prediction reproduces the server's per-tick turbulence **exactly** via the shared deterministic `weatherTurbulence(entityIdx, tickIndex, amp)`; prediction previously predicted zero turbulence and jittered on every gusty reconciliation (#426)
- **render/content**: Livery system — texture-set indirection by material slot (`liveries/<id>.toml`, new `AssetType::Livery`). A livery re-skins an aircraft via `<slot>.<map>` texture overrides with per-map fallback to the base scheme, never touching geometry/nodes/UVs (so user skins are multiplayer-safe); a missing/broken livery degrades to base. Wired through `SceneRenderer::setLiveryResolver`; `validate-livery` (single-file + `--pack`) resolves texture asset names and the aircraft def id; documented in `docs/modding/liveries.md` (#845)

### Changed

- **ai**: Expanded the `intent` eval suite to 111 utterances against the shipped six-command wingman grammar (16 per command + 13 out-of-grammar `unknown` + 5 prompt-injection cases); noted in `docs/ai-provider-evaluation.md` that the shipped grammar is the same size as the placeholder (so the #769 "shorter grammar" latency lever did not materialize) and that re-measurement on the expanded suite is pending a model + reference hardware (#781)

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
