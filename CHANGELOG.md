# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **renderer**: `SceneRenderer` populates `RenderItem::animPoses` (#841, Epic #837) — the renderer knew
  how to be told where an aircraft's parts are; nothing told it. `EntityRenderEntry` gains a normalized
  `artChannels[]` (all-zero is neutral, so an entity nobody articulates renders exactly as before), and
  the entity loop samples every channel the mesh's rig actually models into a **frame pose arena**.
  Spans are patched in after the loop from recorded (offset, count) pairs, which makes a dangling span
  *impossible* rather than merely unlikely — reserving up-front and hoping the estimate holds would let
  one reallocation silently invalidate every span already handed out. An entity whose mesh has no clips
  gets an empty span and the existing single-draw path. Spin (`prop_spin`/`rotor_spin`/`wheel_spin`)
  gets a per-entity phase accumulator held render-side: a propeller's angle is cosmetic, never
  simulated and never wired. A new `art <entityIdx> <channel> <value>` console command scrubs a channel
  end-to-end, so the whole clip → sampler → arena → per-node draw path is demonstrable before the
  simulation or the wire drive it — and stays useful afterwards for telling "the clip is wrong" apart
  from "the sim is wrong".
- **renderer**: articulation rig — channel registry, clip parser and scrub sampler (#840, Epic #837).
  Nothing in the engine knew what an animation clip was: `docs/modding/3d-models.md` promised modders
  an animation-name registry the renderer never read, and `NodePose` had existed with no producer.
  `engine/render/MeshArticulation` implements the contract every shipping sim uses — **the engine owns
  named normalized channels, the model bakes keyframed node-TRS clips, and the runtime SCRUBS at
  `t = value × duration`; it never "plays" a clip.** So retraction is scrubbing `gear` toward 0, not a
  second `gear_retract` clip to keep in sync. Sixteen channels (`gear`/`flaps`/`speedbrake`/`hook`/
  `canopy`/`sweep`/TVC/control surfaces/`prop_spin`/`bay`/gear compression) in an append-only enum
  whose order is the wire order; signed channels centre on the clip midpoint so asymmetric control
  authority is authored as asymmetric endpoints rather than by rescaling the parameter. STEP, LINEAR
  and CUBICSPLINE are evaluated; skins and morph-target `weights` are rejected at parse with a
  diagnostic (rigid parts only); `_b` damage nodes inherit their base node's pose; spin is the one
  looping exception. **A mesh with zero animations builds an empty rig and costs nothing** — the
  static-mesh baseline stays valid forever. Transit timing lives in the simulation, never in the clip.
- **renderer**: entity-selected variant node-sets (#882) — one `.glb` serves a whole airframe family.
  Tag a glTF node with `extras: {"fl_variant": "two_seat"}` (a string or an array) and an entity def
  picks its set with `mesh_variant = "two_seat"`; **untagged nodes are always drawn**, so the shared
  airframe stays shared and every mesh authored before this is unaffected. A pack can now ship one
  MiG-21 mesh serving both the bis and the two-seat U instead of N `.glb` + LOD + damage sets to keep
  in sync. Purely load-time and static — node *presence*, never node *pose* (that is articulation,
  #837): no per-frame cost, and the selector rides `MsgEntityTypeDef` as a tail-append because the
  client has no pack entity def to read it from. `validate-entity --pack` errors when `mesh_variant`
  matches no tag in the referenced mesh and lists the tags the file does declare — without it a typo
  renders as the bare shared airframe with no diagnostic anywhere; `fl-viewer`'s node panel shows each
  node's tags.
- **renderer**: node-aware glTF loader and per-node submesh draws (#839, Epic #837) — the foundation
  articulation, the `_b` damage convention and LOD selection all needed. `createMesh` read only
  `meshes[0].primitives[0]` and ignored node transforms entirely, so a multi-node aircraft was
  impossible and `f5e.glb` rendered correctly only by luck. It now walks the scene graph through a new
  pure, GPU-free `MeshNodePlan` (`platform-meshgraph`, unit-tested without a device), concatenates
  every mesh-bearing node's primitives into the one VB/IB pair, and keeps a per-node submesh table
  keyed by the **glTF node array index** — the contract that lets an engine-side sampler address nodes
  a platform-side loader uploaded. Each draw loop (shadow, forward-opaque, transparent, inset) expands
  an item into per-node draws with `model = transform · Q·G·Q⁻¹`, where `G` is the composed
  content-frame node transform (`RenderItem::animPoses` overriding a node's rest local) and `Q` the
  content→body rotation. `kRenderFlagDamaged` — set since #886 and read by nobody — finally selects
  `_b` submeshes over the base ones they shadow. Per-primitive materials land here too, so a
  multi-material `.glb` no longer loses everything after the first primitive. `FrameStats::drawCalls`
  is now measured rather than assumed to be one per item.
- **tools**: `validate-mod` (#651, Epic #836) — one command validates a whole content pack, so
  fl-base-pack CI runs one gate instead of seven. It composes the existing per-asset validators by
  LINKING their libs (never subprocessing): the manifest (through a new shared `parseModManifest` that
  `ModLoader` now also delegates to, so the loader and the validator cannot drift), an optional
  `[files]` SHA-256 integrity table (the cheap half of #246, via `engine-crypto`), pack-structure
  checks, and every asset through `validateEntityPack` / `validatePackWeapons` / `validateLiveryPack`
  / `validateSensor` / `validateMesh` / `validateFlightModel` / `validateMission` / `validateCampaign`
  / `validateGameMode` / `parseAirportDef` / `parseTheaterManifest` / `validatePlaylist` / REUSE
  licenses. Findings are domain-prefixed. `validate-mod <pack>` passes on fl-base-pack (the Phase 9
  acceptance criterion, mechanized).
- **tools**: `validate-campaign` (#847, Epic #836) — the campaign format's first validator, so a
  campaign author's feedback loop is no longer "the engine failed to load it". It delegates the schema
  to the engine's `parseCampaign` (anti-drift) and, with `--pack`, resolves every theater manifest,
  story/template file, and frontline PNG raster (8-bit grayscale, dimensions == the theater's
  `frontline_grid`) inside the pack. Theater manifests become real: a new `parseTheaterManifest`
  (`theaters/<id>.toml` with geographic `bounds` + an optional `terrain`, default `"world"`), an
  `AssetType::Theater`, an `IContentPack::loadPackFile` for raw pack-relative reads, and a hardened
  8-bit-grayscale `FrontlinePng` codec (its own `engine-campaign-png` target). `parseCampaign` now
  catches dangling `next.id`/`unlock`/`theater` references, duplicate ids, and warns on stories
  unreachable from any trigger. fl-server finally consumes campaign rasters end-to-end: the previously
  unset `FrontlineLoader` decodes them, and theater `bounds` map the raster onto real lat/lon.
- **tools**: `fl-viewer` interactive model viewer (#838, Epic #836) — the DCS-Model-Viewer-shape tool
  every established sim ships. `fl-viewer --entity fl-base:f5e` (or a bare `.glb`) opens a window
  showing the aircraft exactly as the game draws it, with an orbit/pan/zoom camera, a glTF node-tree
  panel (via the new `describeMeshNodes` in `validate-mesh-lib`), inline validate-mesh diagnostics, a
  grid + engine-axis gizmo, damage-mesh toggle, and live hot-reload (#152) — edit the mesh on disk and
  the view updates, the killer iteration feature. New renderer debug views back it: a **wireframe**
  view (a LINE-polygon forward pipeline behind the newly enabled `fillModeNonSolid`) and a
  **normals** view (`shadingMode 5` — visualize the final world-space normal to debug normal maps),
  selectable in snapshot mode too (`--view wireframe|normals`). The IGui HAL gains `checkbox` /
  `treeNode` / `treePop`. The single tinygltf+stb implementation is centralized into a shared
  `tinygltf-impl` target so the viewer can link both the renderer and the mesh-validation lib.
- **content**: asset hot-reload (#152, Epic #836). Editing a mesh, texture, livery, flight model or
  localization file on disk now updates the running game live, no restart — gated behind
  `FL_HOT_RELOAD=1` (all build configs; the env var is inherited by the single-player fl-server
  subprocess, so one variable lights up both halves). A new polling `StdFilesystemWatcher`
  (`platform-stdfs`) is the one production `IFilesystemWatcher` backend (two-scan settle defeats
  partial writes with no timers; rename = delete + create). `AssetManager` gains fine-grained
  eviction (`processHotReload` reports exactly which assets changed via a shared
  `engine/content/AssetPaths` reverse-map, watches asset subdirs not the giant terrain tree, and
  bumps a `cacheGeneration`); `SceneRenderer` gains `invalidateMesh/Texture/Liveries/AllAssets` with
  texture->mesh dependency tracking; `IRenderer::destroyMesh` now cascades to the mesh's material +
  textures so a re-upload cannot leak GPU memory; `FlightIntegrator::setFlightModel` swaps the model
  in place preserving flight state; `WorldBroadcaster` gains `reloadFlightModels`/`replaceController`.
  The `reload_content` console/admin command (previously a stub) forces a full reload on both client
  and server.
- **tools**: `fl-viewer`, a standalone model preview on the game renderer, and the session-free
  preview bootstrap it shares (#666, Epic #836). A new `engine/render/PreviewScene` loads one entity
  def or a bare `.glb` through the real content stack + renderer, resolves its glTF PBR material
  through the same resolver `SceneRenderer` uses (the extracted `render/MeshTextureResolver.h`, so the
  two cannot drift), computes the model bounds (`IRenderer::getMeshBounds`) and frames a camera on
  them. This commit ships `fl-viewer`'s **headless snapshot** mode — `fl-viewer [--entity fl-base:f5e
  | model.glb] [--assets <root>] --snapshot out.png [--size WxH] [--frames N] [--damaged] [--view
  shaded|facecolor] [--require-content]` renders offscreen (no window, no server, no session — the
  gap the game's full-session `--screenshot` left) and writes a PNG, the golden-image CLI pack CI
  needs; run without `--snapshot` it points at the interactive window (#838). The game gains `--size
  WxH` (headless resolution) and a `screenshot [path]` console command. A lavapipe headless snapshot
  smoke runs in CI so the epic's render bootstrap is gated on every PR.

### Changed

- **tools**: `tex-compress` now emits **Basis Universal** KTX2, not raw block-compressed textures
  (#846, Epic #836). This is a correctness fix as much as a portability one: the tool asked toktx for
  `--encode bc7`, but toktx v4 has no raw-BCn encoder and *silently emitted an uncompressed texture*
  (`vkFormat = VK_FORMAT_R8G8B8_SRGB`, not a block format) — so packs were shipping uncompressed,
  desktop-only assets. The engine already transcodes Basis at load (`ktxTexture2_TranscodeBasis` →
  BC7 on desktop / ASTC 4×4 on Apple Silicon), so a Basis KTX2 is the portable form every GPU wants.
  `--type` now selects the encoding — base color → **ETC1S** (small), normal/ORM/emissive → **UASTC**
  (high quality, zstd-supercompressed via `--zcmp`), the split `KHR_texture_basisu` exists for.
  `--format` takes `etc1s|uastc` (the old `bc1|bc3|bc7` values parse as deprecated aliases with a
  warning). The CI smoke now asserts the output is Basis (`vkFormat == 0`), not merely that a file
  appeared. `gen_terrain_color.py` switches its satellite tiles to UASTC.
- **docs**: rewrite `docs/modding/textures.md` around the source-vs-artifact split (#846, Epic #836).
  The guide previously told authors to commit `.ktx2` and discard the source `.png` — backwards, and
  self-contradicted by its own later sections. It now states the industry pattern plainly: **PNG
  masters are the committed source** (under `aircraft/<id>/textures-src/`, the preferred-form-of-
  modification a CC-BY pack must ship), and **`.ktx2` is a git-ignored build artifact** produced by
  `tex-compress` in the pack's release workflow and shipped under `textures/`. A new local-dev section
  documents the engine's `.png` fallback (`textures/<name>.ktx2` → `.png`), so an author can preview
  and iterate — in-game and in `fl-viewer` — with no compressor installed. Straggler fixes: the
  `3d-models.md` example URI depth (`../textures/` → `../../textures/`), and the "ships `.ktx2`" lines
  in `formats.md` / `liveries.md` now say "built from committed PNG masters".

## [0.3.9] - 2026-07-23

### Fixed

- **network**: harden `SpatialIndex` against an extreme-but-finite world coordinate (#993, deep-fuzz
  `fuzz_server_msg`). A hostile `MsgClientInput::cameraEye` survives `WorldBroadcaster::onReceive`'s
  `isfinite()` guard, flows into an observer's interest center, and reaches `SpatialIndex::queryRadius`,
  where `floor(v) -> int64_t` on a value past 2^63 is undefined behavior (UBSan flagged 9.52682e+135).
  A new `cellCoordFloor` saturates the cell coordinate into the `int64_t` range, and the query loop now
  breaks at its bound instead of incrementing past `INT64_MAX` (a second overflow the saturation exposed).
  Both `queryRadius` and `insert` route through it; the common near-origin path is byte-for-byte
  unchanged. Minimized reproducer committed as a `regress-*` fuzz seed plus `SpatialIndex` and
  `WorldBroadcaster` Catch2 regression tests.

### Added

- **game**: game-master overview map and entity orders (#861, completing Epic #851). A new in-flight
  overlay (`GmMapOverlay`, toggled with **M**, gated on the `gm_map` capability) draws the whole
  battlespace top-down from a new `MsgGmWorldState` wire feed — unicast at ~1 Hz only to `gm_map`-capable
  peers from the #600 aggregate, so the map scales to 128 players without per-camera interest. The GM
  clicks to select an entity, issues the six-command flight grammar / forms a flight / kills it through
  the permission-checked admin channel, and drops into any entity's chase view (reusing the #860
  `EntitySelector` plus a `spectate` that re-centres server interest). Map projection and picking live in
  the pure `GmMapView`; `flight order` gains `--target <entityIdx>` so a GM can designate for
  `attack_my_target` from a clicked entity. New rebindable `InputAction::GmMap` (default M). Covered by
  `test_gm_map_view`, `test_gm_map_overlay`, and GM-feed cases in `test_world_broadcaster` /
  `test_client_net_event_handler`.

- **server**: aggregated world-state surface core (part of #600, Epic M #589). New
  `engine/net/WorldState.{h,cpp}` — a plain-data `WorldStateSnapshot` (entities with
  id/gen/faction/type/owner/formation/category/damage/pos/vel/hp, a per-peer summary, weather, time)
  and a pure, deterministic `buildWorldStateSnapshot` builder (SDL/ENet-free, golden-testable) that
  iterates `EntityManager::forEach` in ascending pool order. `WorldBroadcaster` rebuilds it in the
  Serialize phase every ~1 Hz (a bounded sim-thread copy) and exposes `worldState()`. This is the
  single surface designed once for both the Epic M agentic-AI read API (JSON/socket built on top
  later) and the #861 game-master overview map (its first consumer — a 128-player map cannot use
  per-camera interest). Covered by `tests/test_world_state.cpp` and a cadence case in
  `test_world_broadcaster`.

- **netcode**: granted-authority ConnectAck extension and client affordance gating (#949, epic #944).
  New `ExtTag::ConnectAckAuthority = 0x0201` (first tag in the reserved `0x0200–0x02FF` MsgConnectAck
  range): a `{u64 caps, u16 factionIndex}` TLV appended after the entity-type records when a peer holds
  granted capabilities, re-sent on a mid-session grant/revoke (`setPeerAuthority` re-calls
  `sendConnectAck`). Additive — no `kProtocolVersion` bump; old clients skip the unknown tag.
  `ClientNetEventHandler` parses it and exposes `grantedCaps()` / `grantedFactionIndex()` /
  `hasCapability()` for UI gating (the server remains the enforcement point). The multiplayer client now
  wires its admin-command sender unconditionally, so a granted-but-passwordless peer can issue the
  commands its capabilities permit through the empty-token grant channel. `docs/network-protocol.md`
  documents the tag and the future requested-authority claim. Covered by round-trip tests in
  `test_client_net_event_handler` and `test_world_broadcaster`.

- **server**: runtime `grant` / `revoke` admin commands (part of #947, rung 2 of the #944 grant
  ladder). `grant <peerId> <admin|moderator|gm|faction_leader> [factionIndex]` and `revoke <peerId>`
  (both require `grant_roles`) set/clear a peer's `PeerAuthority` via `enqueueSimCallback` with a
  synchronous ack (the `set_role` pattern); grants are ephemeral (lost on disconnect — persistence is
  the identity-bound issue #950). The `peers` listing gains a `role=` column showing a granted peer's
  preset. A granted peer authenticates on the admin channel with an empty token and is permission-
  checked against its caps; a moderator cannot self-elevate (it lacks `grant_roles`). Covered by
  grant/revoke and grant→act→revoke→refused cases in `test_admin_console`.

- **network**: permission-checked admin command dispatch with issuer context (#946, epic #944).
  `CommandRegistry::registerCommand` gains a required-capability mask (an unannotated command defaults
  to Admin-only, preserving today's semantics) and a new `dispatch(line, CommandIssuer)` overload that
  refuses with a clear "permission denied: <cmd> requires <cap>" when the issuer lacks a required
  capability; the plain `dispatch(line)` stays the implicit-Admin path (stdin console / RCON /
  single-player `--admin-token`). Every command in `ServerCommands.cpp` is annotated (kick/ban →
  `kick_ban`, spawn/kill/tp/detonate → `spawn_any`, flight orders → `command_any_ai`, weather/config/
  shutdown → `server_config`, spectate → `spectate_any`, grant-family → `grant_roles`, status/peers/
  help → public). `WorldBroadcaster`'s `MsgAdminCommand` handler now builds a `CommandIssuer`: the
  operator password grants Admin caps (rung 1, byte-for-byte today's behavior), while an empty-token
  peer authenticates by its granted caps (the grant channel) — a zero-cap peer is a rate-limited
  permission refusal, never an auth-lockout failure. New `WorldBroadcaster::setPeerAuthority` /
  `getPeerAuthority`. Covered by reduced-caps issuer fixtures in `test_admin_console` and grant-channel
  cases in `test_world_broadcaster`.

- **network**: server capability vocabulary and per-peer authority (#945, epic #944). A new
  stdlib-only `engine/net/Capability.h` (the `WingmanCommand.h` pattern — engine, fl-server, and the
  game client include it with no link dependency) defines `enum class Capability` (bit positions:
  `kick_ban`, `mute`, `server_config`, `spawn_any`, `spawn_faction`, `command_any_ai`,
  `command_faction_ai`, `faction_posture`, `gm_map`, `gm_map_faction`, `spectate_any`, `grant_roles`),
  a `CapabilityMask` (uint64), the `Admin`/`Moderator`/`GameMaster`/`FactionLeader` role presets,
  `parseRolePreset`, capability-name/missing-name helpers, and a `PeerAuthority {caps, factionIndex}`
  added to `PeerInputState` — orthogonal to `PeerRole` (embodiment), default zero caps, erased with the
  peer on disconnect. Foundation only: zero behavior change (nothing reads the field yet). Covered by
  `tests/test_capability.cpp`.

- **platform/game**: force-feedback joystick effects via SDL3 haptics (#928, Epic #587). `IJoystick`
  gains a force-feedback surface as non-pure, no-op-default virtuals (`supportsForceFeedback`,
  `playFfbEffect(slot, FfbEffect{ConstantForce|Sine, direction, magnitude, period, duration})`,
  `stopFfbEffect`, `stopAllFfbEffects`; `kFfbSlotCount = 4`) — gamepad-only backends and test mocks
  compile unchanged. `SDL3Joystick` opens a haptic device per FFB stick (`SDL_INIT_HAPTIC` +
  `SDL_OpenHapticFromJoystick`) and creates/updates/runs `SDL_HAPTIC_CONSTANT`/`SDL_HAPTIC_SINE`
  effects; `HapticController` drives stall buffet (slot 0), ground roll (slot 1), and a gun-fire kick
  (slot 2) alongside the existing rumble, capability-guarded and off without a device. Explicitly a
  cueing layer, not control loading (no trim/spring forces). Config `[controls] ffb_enabled` /
  `ffb_strength`; `docs/haptics.md` extended. Covered by a `TrackingJoystick` in
  `tests/test_haptic_controller.cpp`.

- **game**: head tracking via the opentrack UDP protocol (#927, Epic #587). A new `fl::HeadTracker`
  (game layer) opens a non-blocking localhost UDP socket and reads the opentrack 6-double datagram
  (x/y/z cm + yaw/pitch/roll deg); the parse and an EMA + freshness `HeadPoseFilter` are pure functions
  in the header (unit-tested socketless). `CameraInput::setHeadPose` composes the smoothed pose
  additively with the RMB cockpit look — head roll tilts the horizon, a body-frame eye offset lets the
  pilot lean (clamped ±0.5 m). New `[headtracking]` `UserConfig` section (enabled/port/scales/inverts/
  smoothing); the tracker starts per session when enabled and closes on teardown. Covered by
  `tests/test_head_tracker.cpp` (datagram size validation, deg→rad/cm→m mapping, scale/invert/clamp,
  EMA convergence, freshness timeout).

- **renderer/game**: NVG cockpit overlay (#210, Epic #587). Night-vision goggles as a tonemap-stage
  green photocathode gain rather than a flat overlay: `TonemapPush` gains an `nvgIntensity` field
  (consuming a pad slot, size unchanged) and `tonemap.frag` a soft-knee luminance amplification painted
  P43 phosphor green, so dim starlit terrain reads and bright sources bloom through the existing bloom
  path. New non-pure `IRenderer::setNightVision(float)` (no-op default — mocks unchanged) with a
  `VkRenderer` override. Toggled with the new `NvgToggle` action (F7), cockpit-only, driving the render
  loop each frame and an `NVG` HUD annunciator. There is no client-side visibility penalty to suppress —
  the brightening is the mechanic. Mocks compile untouched; the `TonemapPush` size asserts and the
  input-default test cover the ABI + binding.

- **game**: radar MFD and RWR display pages (#642, Epic #587). The datalink scope (#528) becomes a
  cyclable MFD: `HudMfdState` (owned by `FlightScreen`, cycled with the new `MfdPage`/`MfdRange` actions
  on O/3) selects an Off page, the 360° PPI, a B-scope (azimuth ±60° vs range), or a dedicated RWR
  threat-warning ring, with IFF-coloured contacts, a range caption, and a SIL/SRCH/TWS/STT radar-mode
  annunciation from the requested mode. The `RWR LAUNCH`/`RWR LOCK` threat caption is never page-gated
  (shows even on the Off page). Rendered entirely through the existing `HudElement` overlay path
  (IGui-independent). Covered by new cases in `tests/test_flight_hud.cpp` (page/range cycling, the
  never-gated caption, B-scope geometry).

- **game**: combat HUD modes — target box, pipper, CCIP, weapon status, master arm (#641, Epic #587).
  `FlightHud::drawCombat` renders the gun pipper (ballistic lead via `computeBallisticLead`, gravity
  from the local radial up), the CCIP bomb-release cross + fall line + time-of-fall (`computeCcip`
  against the terrain height query, capped at 30 s), and a lower-right multi-station weapon-status
  block (selected station bracketed with its live round count; unselected stations show `x--` pending
  the per-station-ammo wire follow-up). The designator box (drawn by `FlightScreen`) gains IFF colour
  via the #688 helper plus range + closure. Master arm is a real V-key toggle in `FlightInputCollector`
  that suppresses the gun + fire-store trigger bits and `wasWeaponFired` when SAFE (new `MasterArm`
  input action, default V). Pipper/CCIP are gated by master arm and the selected station's weapon kind
  (from `HudStationInfo`). Covered by new cases in `tests/test_flight_hud.cpp` and
  `tests/test_flight_input_collector.cpp`.

- **game**: target-slaved inset view on the HUD (#698, Epic #587). A small live 3D repeater of the
  designated target, toggled with `TargetInsetToggle` (F6) and framed bottom-centre. `CameraView`
  construction is extracted into a shared `makeCameraView()` free function (`CameraController::view()`
  delegates to it) so the inset cannot diverge from the main projection conventions; a pure
  `game/fighters-legacy/InsetViewMath.h` builds the inset camera (eye on the target→ownship line at a
  30 m stand-off, look-at the extrapolated target, degenerate-safe) and the square-pixel bottom-centre
  rect. `SceneRenderer::setInsetView` writes the #695 `FrameScene` inset fields; `FlightScreen` toggles
  it, feeds the camera, and draws the border, auto-hiding when the designation clears. Works alongside
  padlock. Covered by `tests/test_inset_view.cpp`.

- **game**: padlock camera mode with LOS lock-break and re-acquisition (#697, Epic #587). New
  `CameraMode::Padlock` (F5): the camera stays at the cockpit eye and slews to keep the designated
  target centred. All the math is a pure `fl::PadlockTracker` — a continuous world-space aim slewed
  toward the extrapolated target with a damped, rate-limited (240°/s) approach and an elevation clamp
  (overhead crossings resolve smoothly, never a 2π jump), a cockpit visibility envelope, and a
  Locked → Breaking (0.4 s grace) → Reacquire (4 s) → Off state machine. `CameraInput` drives it,
  latching terrain LOS (#687) at ~15 Hz; `FlightScreen` toggles it (auto-designating best-in-cone via
  #696 when nothing is designated), shows the `PADLOCK`/`PADLOCK — BREAK`/`REACQ` cue, and treats
  Padlock like Cockpit for ownship-hiding + HUD gating. Frame-rate-independent by construction; covered
  by `tests/test_padlock_tracker.cpp` (overhead-pass continuity, 60-vs-240-fps equivalence, grace
  hysteresis, reacquire expiry, break-slew return, envelope masking).

- **renderer**: secondary-camera inset viewport via `FrameScene` (#695, Epic #587). `FrameScene` gains
  POD `insetEnabled`/`insetCamera`/`insetRect` fields (no new `IRenderer` pure virtuals — mocks and every
  `setScene` consumer compile unchanged), and `VkRenderer` renders the same scene a second time from the
  inset camera into a scissored sub-rect of the HDR target with its own per-frame camera UBO/descriptor
  set. The camera-relative rebase invariant is preserved by composing the inset view with
  `translate(mainOrigin − insetOrigin)` in double precision. The disabled path is bit-identical to
  before (default-off asserted in `tests/test_scene_renderer.cpp`); sky/transparents/per-inset GTAO are
  v1 out-of-scope. Backs the #698 target-slaved inset.

- **game**: client-side target designation with next/prev cycling (#696, Epic #587). A new
  `fl::TargetDesignation` (game layer) is the single client-side source of truth for the designated
  target: cycle with the `NextTarget`/`PrevTarget` actions (N/P), resolve auto-clears on despawn /
  generation mismatch / death, and a pluggable candidate provider (the #526 sensor-track upgrade seam)
  defaults to a snapshot scan that filters out projectiles/effects/self/destroyed and orders hostile-
  first then by range, plus `designateBest` (best-in-forward-cone with nearest fallback) for padlock's
  toggle-on. Wired into `FlightScreen` with a minimal designator box + `TGT <type> <range>` cue
  projected via `HudProjection`; operates purely on `RenderSnapshot`, so replay inherits it. Also
  raises `FlightScreen`'s element cap to match the redesigned HUD. Covered by
  `tests/test_target_designation.cpp`.

- **game**: player autopilot modes — altitude / heading / speed hold (#640, Epic #587). A new pure
  `fl::Autopilot` (game layer) shapes the client's input before it is sent, so the server stays
  authoritative and dumb: each hold captures its target on engage, `compute()` drives elevator/aileron/
  rudder/throttle via the engine's P-controller primitives (`Guidance.h`/`LocalFrame.h`, the same laws
  the AI flies), and any stick input past a threshold disengages the attitude holds while a throttle
  touch drops speed hold. Wired into `FlightScreen::update` after `poll()` and before prediction+send
  (so the client predicts exactly what the server receives), toggled by new `AutopilotAltHold/HdgHold/
  SpdHold` actions (F9/F10/F11 — A/D/S collide with the active roll/pitch bindings), and annunciated on
  the HUD via the `HudFrameInput` autopilot fields. Covered by `tests/test_autopilot.cpp` (sign checks,
  disengage matrix, and a closed-loop run through the real `FlightIntegrator`).

- **renderer**: redesigned FlightHud to a tactical fighter HUD layout (#438, Epic #587). The minimal
  text HUD is replaced by an F-14/F-16/F-18-style layout built entirely from `HudElement`: a velocity
  ladder (knots) on the left, an altitude tape (feet) + radar-altitude on the right, dual heading
  tapes with cardinal labels and a lubber line, a velocity-vector flight-path marker (projected via
  `HudProjection` #692) with the radial artificial horizon, lower AoA/Mach/G/fuel and weapon/master-arm
  blocks, and an octagonal combiner frame — plus the relocated datalink radar/RWR MFD. `IHud::update`
  now takes a single `HudFrameInput` bundle (changed once here; the combat/MFD/autopilot consumers add
  defaulted fields rather than re-churning the signature), and `FlightHud::setStationInfo` replaces
  `setStationLabels`, carrying muzzle velocity + weapon kind for the coming pipper. Element/string caps
  raised with an `overflowed()` silent-truncation guard. Draw code split into
  `FlightHud`/`FlightHudCombat`/`FlightHudMfd`. `FlightScreen` computes the frame's `CameraView` for the
  HUD (D1 seam). Covered by a rewritten `tests/test_flight_hud.cpp`.

- **engine**: world-to-HUD projection helper (#692, Epic #587). New `engine/render/HudProjection.h/.cpp`
  — `worldToHud(CameraView, dvec3)` rebases in double, transforms through the live projection, rejects
  behind-camera points via `clip.w <= 0` (the correct test for the infinite reverse-Z projection), and
  maps to normalized top-left-origin HUD coordinates with the Vulkan Y-flip already baked in; plus
  `hudBox` (four `Line`s — the #641 designator) and `hudAspect` (recovers the viewport aspect, retiring
  hard-coded 16/9). Serves padlock cues, the combat-HUD symbology, and target labels. Covered by
  `tests/test_hud_projection.cpp` with golden values built from `CameraController::view()`.

- **engine/game**: padlock/target-cycle input actions and camera-key unification (#689, Epic #587).
  New `InputAction`s `CameraCockpit`/`CameraChase`/`CameraFree` (default F1/F2/F4), `PadlockToggle`
  (F5, gamepad RightStick), `TargetInsetToggle` (F6), `NextTarget`/`PrevTarget` (N/P, gamepad DpadUp),
  and defaults for the previously-unbound `View*` cockpit-pan actions (PageUp/PageDown/←/→). A new
  header-only `engine/input/BindingQuery.h` (`bindingDown`/`bindingJustPressed`) resolves any `Binding`
  source against `IInput`. `CameraInput` now switches camera modes through `InputBindings` (rebindable,
  gamepad-capable) instead of raw SDL scancodes and pans the cockpit view from the `View*` actions;
  the console-toggle and F3 overlay stay raw. `keyName`/`keyFromName` gained the missing `Minus`/`Equals`
  entries. Keymap documented in `docs/sandbox.md`; covered by new cases in `tests/test_input.cpp`.

- **game**: client-side IFF for arbitrary snapshot entities (#688, Epic #587). `ClientNetEventHandler`
  gains `ownFactionIndex()` (derived from the assigned aircraft's cached faction, roster fallback) and
  `identForEntity(idx, gen, factionIndex)` — a three-step friend/foe resolution (same faction → Friend;
  a live datalink track's server-computed `ident`; else the `areFactionsHostile` affiliation rule).
  Serves the #641 combat-HUD target box and #696 target cycling. No wire change: the faction
  relationship matrix is Lua-mutable and stays server-side, per the `RadarView` `ident` doctrine.
  Covered by new cases in `tests/test_client_net_event_handler.cpp`.

- **engine**: terrain line-of-sight segment query (#687, Epic #587 padlock family). New header-only
  `engine/spatial/LineOfSight.h` — `terrainLos(a, b, heightFn, readyFn, planetRadiusM, stepM,
  clearanceM)` returns `LosResult::{Clear, Blocked, Unknown}` by marching the segment with an
  adaptive, clearance-margin-bounded step (densifies at grazing tangents, stretches over valleys, no
  heap). The heightfield is an injected callable so both the client (`TerrainStreamer::heightAt`) and
  the server share one utility; `Unknown` surfaces when a tile is unloaded mid-segment and is left to
  caller policy (padlock treats it as Clear). `engine-spatial` stays glm-free/pure-stdlib. Covered by
  `tests/test_los.cpp` (ridge/valley/bowl, grazing tangents, step-size independence, Unknown
  propagation, endpoints/zero-length/vertical).

- **network**: fl-lobby registration client and in-game server browser (#143, Epic E #497). The HTTP HAL
  gains a `request()` primitive with POST/PUT/DELETE methods + body/content-type (`get()` forwards to it);
  `LobbyRegistration` heartbeats a dedicated server to an `fl-lobby` service (`POST /v1/servers` on an
  interval, `DELETE` on shutdown, exponential backoff on failure) when `[lobby] register` is set and a
  libcurl backend is present. The client gains a `ServerBrowserScreen` (an IGui list) backed by a pure
  `ServerBrowserModel` that merges LAN discovery, lobby listings (`LobbyListClient` +
  `parseLobbyServerList`, a tolerant bounded JSON scanner), and live server-info query results (ping /
  fresh counts), deduped by `host:port` with LAN winning; selecting a row or Direct Connect prefills the
  join form. The multiplayer main menu's "Join Server" now opens the browser. New `[client] lobby_urls`
  (comma-separated, empty by default — federation posture). The REST contract is documented in the new
  `docs/lobby-api.md` (the fl-lobby Go service, #999, is written from it). Covered by
  `tests/test_lobby_list_parser.cpp`, `tests/test_lobby_registration.cpp`,
  `tests/test_server_browser_model.cpp`, `tests/test_server_browser_screen.cpp`, and a new
  `fuzz/fuzz_lobby_list.cpp` harness.

- **i18n**: localize connection and session-failure strings (#358, Epic E #497). The game client now
  constructs `Localization` at startup from `[client] language` (default `en`) and routes the loading
  screen's session-failure text through a new `tr(loc, key, builtin)` helper that falls back to the
  built-in English string when a key is missing or no locale is loaded — a partial translation degrades
  to English rather than showing raw keys. Each `SessionFailure` enumerator gains a stable
  `sessionFailureKey()`, and `locale/en/ui.toml` is populated with the `[session]`, `[chat]`,
  `[scoreboard]`, `[join]`, `[spectate]`, `[debrief]`, and `[browser]` sections. `tests/test_session_i18n.cpp`
  guards against enum/TOML drift (every failure key present + non-empty in the repo locale, tr fallback).
  Documented in `docs/modding/localization.md`.

- **match**: objective scoring channel from missions to the match controller (#1000, Epic E #497).
  `MatchController::recordObjective(faction, count)` awards `count × points_per_objective` to a team
  during the Active phase (frozen otherwise, inert when the mode declares no objective points). A mission
  awards it from a Lua trigger action via a new `world.score_objective(faction, count)` binding (routed
  through the `WorldApi::scoreObjective` host seam to the match controller in fl-server), and the updated
  `MsgMatchState` broadcast carries the new team scores to clients. Ships the objective-scored
  **`builtin:strike`** game mode (two teams, kill = 1, objective = 10, score limit 100, 20-minute clock).
  Covered by new `tests/test_match_controller.cpp`, `tests/test_game_mode.cpp`, and
  `tests/test_lua_controller.cpp` cases; documented in `docs/modding/game-modes.md`.

- **network**: spectator interest position for dead/observer peers (#403, Epic E #497). A dead pilot
  awaiting respawn no longer gets a blacked-out header-only snapshot — the server seeds its interest
  center from the wreck on death and thereafter follows its camera-eye stream (#858), so it spectates
  the world around it. A new admin `spectate <peer> <entityIdx|off>` command (and
  `WorldBroadcaster::setSpectateTarget`) locks a spectator's view onto a chosen entity (auto-clearing
  when that entity dies). An optional `[world] spectate_delay_s` (0 = off) buffers a spectator's
  positional snapshots by N seconds as anti-ghosting — reliable channels (chat/kill feed/match state)
  stay live — capped at 4 MB/peer and cleared on respawn/role change/disconnect. Client-side, a downed
  pilot drops into a free ghost camera at the wreck and can cycle live entities with the observer picker
  (Num1/Num2), returning to the cockpit on the respawn ack. Covered by new
  `tests/test_world_broadcaster.cpp` and `tests/test_admin_console.cpp` cases.

- **network**: in-match text chat (#646, Epic E #497). New `MsgChat` (0x20, client→server) and
  `MsgChatEvent` (0x21, server→client) wire messages plus a `ChatChannel` (All/Team) enum. The server
  sanitizes each line (BMP UTF-8, control characters stripped, codepoint-boundary truncation at 240
  bytes), rate-limits per peer (`[chat] rate_limit_per_s`, warn-once-per-window), applies a per-session
  admin mute (`mute` / `unmute` / `mutes` commands) and a moderation hook (fl-server default logs an
  audit line and allows), then routes the line — All to every handshake-complete peer including the
  sender's echo, Team to the sender's faction only. The client sends via a chat input box (`Y` = all,
  `H` = team) that captures the keyboard while open (a new `textEntry` gate on `FlightInputCollector::poll`
  suppresses flight keys but keeps the gamepad/HOTAS axes live), and displays a bottom-left fading ring of
  recent lines with local per-callsign muting. `[chat] enabled` toggles the whole channel. New
  `ChatOverlay`, `InputAction::ChatAll`/`ChatTeam`, and `[chat]` config section. Unit-tested end to end
  (`tests/test_chat_overlay.cpp`, server routing/mute/sanitize/rate-limit/hook cases in
  `tests/test_world_broadcaster.cpp`, send/receive in `tests/test_client_net_event_handler.cpp`, admin
  commands in `tests/test_admin_console.cpp`).

- **game**: multiplayer kill feed and scoreboard (#647, Epic E #497). The client now decodes the
  server's `MsgMatchState` (0x1D, match phase + limits + per-team scores) and `MsgScoreboard` (0x1E,
  per-participant kills/deaths/score/ping, upserted across the unreliable chunked stream and pruned when a
  participant leaves the roster). A top-right `KillFeed` HudElement overlay renders recent "X destroyed Y"
  lines — names resolved from the match roster, tinted green/red when you score/take the kill — replacing
  the anonymous "peer N" kill line. A `ScoreboardOverlay` IGui table (held on the Scoreboard key, default
  `I`, and auto-shown in the match end phase) groups players by team with the authoritative match score,
  a phase/countdown header, and a highlighted self row. The debrief adds a winner/draw banner and per-team
  final scores above the personal tallies. New `InputAction::Scoreboard`. Unit-tested against the scripted
  NullGui and a deterministic clock (`tests/test_kill_feed.cpp`, `tests/test_scoreboard_overlay.cpp`, plus
  `MsgMatchState`/`MsgScoreboard` decode + kill-feed name cases in `tests/test_client_net_event_handler.cpp`).

- **game**: multiplayer join-server screen (#322, Epic E #497). The first IGui consumer — a direct-connect
  form with real text fields for the server address (`host[:port]`), an optional join password (#998,
  masked) and a callsign (pre-filled from the pilot profile). Connect (button or Enter) parses the address,
  writes the session's connect parameters into the game's Services state (the same fields `--connect` sets)
  + records an edited callsign, then transitions to Loading so the existing session machinery connects over
  GNS with the password; Cancel/Escape returns to the main menu. The multiplayer main menu's "Join Server"
  item now opens this form instead of connecting blind. Fully unit-tested against the scripted NullGui
  (`tests/test_join_server_screen.cpp`).

- **ui**: IGui HAL with a Dear ImGui reference backend (#156, from epic #593; the foundation for Epic E
  #497's input-heavy multiplayer screens). A narrow immediate-mode GUI interface (`platform/IGui.h` —
  windows, labels, buttons, masked text input, selectable rows, read-only tables, input-capture flags)
  that screens speak instead of coupling to a GUI toolkit, so they stay backend-agnostic and unit-testable
  against the scripted `NullGui` (`tests/mock_gui.h`). The reference backend (`platform-gui`, `ImGuiGui`)
  implements it over Dear ImGui + the SDL3/Vulkan backends: the ImGui context + SDL3 platform backend live
  in `platform-gui`, while the ImGui Vulkan renderer backend runs inside `VkRenderer` (the only holder of
  the Vulkan handles) via two `IRenderer` hooks, recording draw data into the swapchain pass after the
  HudElement overlay. Dear ImGui is FetchContent-pinned (v1.91.5, MIT) and Vulkan-gated; the module-layering
  guard forbids `engine-*`/`fl-server` from reaching `platform-gui`/`imgui`. Wired into the game's frame
  loop (SDL events forwarded to the GUI first via `IWindow::setGuiEventForwarder`; `newFrame()`/`render()`
  bracket each frame). The real backend needs a GPU (visual-check verified); the HAL contract is CI-tested
  through `NullGui`.

- **network**: server info query protocol (#997, Epic E #497). An A2S-style request/response over raw
  UDP on a dedicated query port (`[discovery] query_port`, default game port + 1) gives the server
  browser a ping measurement + live details for a server. `MsgServerQuery` (0x41, padded ≥ response for
  anti-amplification) / `MsgServerInfo` (0x42, echoes the nonce/timestamp). `ServerQueryResponder`
  (own thread, per-IP + global rate limiting) answers on the server; `ServerQueryClient` (main-thread
  poll, RTT from a local nonce→send-time map) queries from the browser. The LAN beacon advertises the
  query port (`MsgLanBeacon` 76 → 78 B). fl-server starts the responder and refreshes its dynamic snapshot.
- **server**: AI bot backfill (#87, Epic E #497). A new `[bots]` config (`fill` / `max_bots` /
  `entity_type` / `ai_script` / `balance_teams`) fills a team match with server-side AI participants up
  to a target count — bots are not network peers. `WorldBroadcaster::registerBotParticipant` /
  `removeBotParticipant` give each bot a scoreboard row (badged, ping 0) and route its kills/deaths
  through the participant model; a `BotRoster` (fl-server) spawns `builtin:fighter` bots on the smaller
  team, retires/backfills to track the human count, reaps killed bots, and clears on rotation. Pure
  `desiredBots` fill math in `engine-match`.
- **docs**: game-mode authoring guide (#525, Epic E #497). New
  `docs/modding/game-modes.md` documents the `modes/*.toml` schema, the builtin modes, mode selection
  (`[match] mode` / `mission@mode`), the match lifecycle, and `validate-mode`. `docs/fl-server-config.md`
  gains the `[match]` section (mode / end_screen_s / reconnect_grace_s) and documents the now-enforced
  `[server] password`; `docs/network-protocol.md` documents the new Epic E message ids, the raw-UDP
  boundary raise to `0x40`, and the beacon shutdown/passworded flags.
- **server**: join password for private servers (#998, Epic E #497). The existing (previously inert)
  `[server] password` is now enforced: a connecting client sends it as a `MsgConnectRequest` TLV
  (`ExtTag::ConnectJoinPassword`), the server constant-time-compares it (before any admission side
  effect, for both pilots and observers) and refuses a missing/wrong password with the new
  `ConnectRefusalCode::BadPassword` → `SessionFailure::BadPassword`. The LAN beacon advertises a
  `kGameModePassworded` flag so a browser can show a lock icon and prompt. The client carries a
  per-session join password (not persisted).
- **net**: reconnect identity with score/team restore inside a grace window (#524, Epic E #497). The
  client sends its `PilotProfile::guid` as a `MsgConnectRequest` TLV (`ExtTag::ConnectIdentity`); on
  disconnect the server snapshots the player's callsign, team and kills/losses/score under that GUID and
  restores them on reconnect within `[match] reconnect_grace_s` (default 120 s) — the reconnector rejoins
  their old team rather than being re-balanced. A GUID already held by a live peer is treated as fresh;
  entries expire lazily + on a periodic sweep, and are cleared on match rotation. The aircraft is not
  kept alive through the window (the player respawns per the mode policy).
- **network**: advertise shutdown state in the LAN discovery beacon (#226, Epic E #497).
  `MsgLanBeacon` gains a `kGameModeShuttingDown` flag + a `shutdownSeconds` field (74 → 76 B);
  `WorldBroadcaster::getShutdownStatus()` publishes the countdown across threads (relaxed atomics) for
  the main-thread beacon, which now sends an immediate extra beacon on a shutdown state change so a
  server browser sees "shutting down (m:ss)" within one poll. `DiscoveryListener::ServerInfo` exposes
  `shuttingDown()` / `shutdownSeconds()`.
- **net**: death → respawn state machine with mode-defined delay (#648, Epic E #497). A dead pilot is
  enrolled in a per-participant respawn table (`WorldBroadcaster::setRespawnPolicy` from the game mode:
  delay, optional waves) and respawns on request — the new `kInputButtonRespawn` (MsgClientInput bit 7,
  edge-detected; default key Backspace) for humans, automatically for bots (#87) — once the delay
  elapses and combat is not frozen. `respawnParticipant` + an admin `respawn <peerId>` command force it;
  the entity teardown is deferred out of the damage event to a per-tick `processRespawns`. Replaces the
  old behavior where a killed peer's aircraft simply disappeared until reconnect.
- **match**: match lifecycle, team scoring, and in-process rotation (#523, Epic E #497). A new
  `MatchController` runs the deterministic Idle→Warmup→Active→Ending→PostMatch state machine (score /
  time limits, warmup gated on min players, mission-driven `forceEnd`, a rotate hook). `WorldBroadcaster`
  broadcasts the new reliable `MsgMatchState` (phase, per-team scores, limits, phase clock) on change
  and to late joiners, and the unreliable `MsgScoreboard` (per-participant kills/deaths/score/ping)
  every ~2 s; a combat-freeze gate suppresses fire input and scoring during Ending/PostMatch; and
  `resetWorld` + `readmitPilots` rotate the match in-process (peers stay connected) so the server no
  longer sits idle after a mission ends. fl-server owns the controller, feeds it kills + participant
  join/leave through new sinks, and cycles the `[rotation]` mode on `[match] end_screen_s`.
- **match**: team assignment, balancing, and mode-driven friendly fire (#522, Epic E #497). A pure
  `TeamBalancer` (`pickTeam` / `switchAllowed`) drives team choice; `WorldBroadcaster` gains a
  `setTeamAssigner` seam (consulted at admission — `nullopt` refuses with the new
  `ConnectRefusalCode::MatchFull`), faction-preferred mission-slot claiming, `factionForPeer`, and a
  `setPeerFaction` despawn-and-respawn path. Clients can request a mid-match switch via the new
  reliable `MsgTeamRequest` (guarded against unbalancing); an admin `team <peer> <faction>` command
  bypasses the guard. fl-server maps a mode's teams onto the FactionRegistry (mission sides, positional
  aliasing, or a synthesized zero-pack registry) and applies the mode's friendly-fire override over
  `[gameplay] friendly_fire`. Unset assigner ⇒ the legacy single-faction behavior, unchanged.
- **match**: data-driven game-mode framework (#521, Epic E #497). A new `engine-match` library defines
  `GameModeDef` (teams, scoring, respawn policy, win conditions, warmup, friendly-fire override) and
  `parseGameModeToml`, the single parser both the engine and the new `validate-mode` tool call. Two
  compiled-in modes ship — `builtin:free-flight` (the default; byte-identical to today's sandbox) and
  `builtin:tdm` — plus a `modes/*.toml` content-pack asset type (`AssetType::GameMode`). fl-server
  selects a mode via `[match] mode` or a rotation item's `mission@mode` suffix and resolves it at
  startup (builtin id → pack asset → free-flight fallback); the MatchController consumes it in #523.
- **network**: player callsigns, match roster broadcast, and ENet id-space groundwork (#996, Epic E
  #497). `MsgConnectRequest` gains a fixed `callsign[32]` field (from `PilotProfile::callsign`, server-
  sanitized) and `MsgConnectAck` a `peerId` tail field so a client learns its own participant id. A new
  reliable `MsgPlayerRoster` upsert/leave stream (`0x1C`) carries participant id → callsign / faction /
  role, broadcast on join / role change / leave and sent in full to a late joiner; the client resolves
  it through `ClientNetEventHandler::displayName()`, the single name source for chat, kill feed and
  scoreboard. Establishes the participant-id model (humans = peerId, bots = `kBotParticipantBase + n`).
  The raw-UDP `MsgId` boundary is raised `0x20` → `0x40` (`MsgLanBeacon` → `0x40`), freeing `0x1C`–`0x3F`
  for the Epic E messages (roster/match state/scoreboard/team/chat).
- **audio**: RWR and missile-lock warning tones (#960, Epic #586). `WarningToneManager` gains a
  radar-warning-receiver channel — a slow search strobe, a steady lock tone, and a fast launch warble
  — driven by the peer's LEGITIMATE threat picture (the datalink/RWR strobes the server decided it
  detects, so nothing leaks that the RWR did not honestly hear). One voice plays the worst hostile
  level; escalation is instant (a launch is heard the frame it appears) and de-escalation runs through
  a hold so a one-frame gap does not chop the tone. The RWR honors its own `AudioSettings::rwrVolume`
  slider (previously declared but unconsumed). A new `sensor::ThreatLevel::Launch` is sourced honestly
  from live RADAR-guided missiles guiding on the peer (via a `SensorSystem` missile-threat provider
  seam the `WorldBroadcaster` wires to the projectile pool) — IR-seeker missiles are passive and never
  light the RWR, and a coasting/lost seeker drops the strobe, rewarding a defeated lock. Carried on the
  existing `MsgDatalink` threat record (no wire size change); the HUD radar scope shows a distinct
  `RWR LAUNCH` caption. State machine unit-tested headless (priority, hysteresis, `rwrVolume`, launch
  injection + ordering).
- **audio**: continuous engine and aerodynamic sound layers with positional doppler (#959, Epic #586).
  A new `engine/audio/EngineAudio` (engine-audio) holds the byte-stable builtin engine hum + airframe
  wind-rush loops and the pure throttle/airspeed → pitch/gain mapping; the game-layer
  `EngineAudioManager` (beside `ClientEffectRouter`) drives them off the render snapshot each frame — the
  ownship's engine is HEAD-LOCKED (no distance falloff, no doppler on your own jet) while every other
  air entity gets a POSITIONAL looping engine source with its world velocity set so OpenAL applies
  doppler on a flyby, distance-attenuated and capped to the nearest eight. The engine pitch/gain track
  throttle, airspeed and afterburner; the wind rush swells with dynamic pressure. Only air vehicles hum
  (category predicate). A null audio device is a silent no-op, so CI opens no device. Unit-tested headless
  via a tracking `IAudio` (mapping, byte-stable PCM, head-lock/doppler/distance wiring).

## [0.3.8] - 2026-07-20

### Added

- **mission**: per-epic cinematic demo missions — one per engine epic landed for v0.4.0 (#909).
  `missions/demos/` gains five zero-content-pack scripted demos with `cameras:` shots, each showcasing
  one epic: `demo-bomber-defense` (multi-crew tail gunners, #966), `demo-carrier-swarm` (carrier vessel
  model + naval escorts + a drone boids swarm, #585), `demo-atc-scramble` (ATC-sequenced departures from
  the builtin airfield, #673), `demo-ejection` (a scripted `detonate` flak barrage that wounds a lead
  into the auto-eject band so a replicating parachute appears, #584), and `demo-night-patrol` (a night
  CAP over the airfield under the geographically-correct star field + Moon, #468). All ten demos are
  registered in `demos.json`; camera coords were tuned against deterministic `--mission-report` traces.
  `docs/demo-recording.md` documents the set + the roster-id/fixed-point `look_at` rule, and
  `docs/modding/missions.md` adds `detonate`/`atc_scramble` trigger examples. Carrier catapult/recovery
  and rotorcraft are documented as out-of-scope until a content pack provides the aircraft.
- **renderer**: parachute placeholder silhouette (#672). `builtinShapeFor` maps `ObjectCategory::Effect`
  to a new `BuiltinShape::Parachute` (a canopy dome + risers + pilot, generated by `gen_builtin_glb.py`,
  validate-mesh clean) instead of the Unknown error beacon — the ejected-pilot chute is the one Effect
  entity that replicates and must render, so it now shows a real chute rather than the bug caltrop.
- **engine**: `GameLoop::drainSimCallbacks()` + a faithful `--mission-report` (#909). The deterministic
  mission-report loop now drains queued sim callbacks each tick before stepping, so mission `do:` trigger
  actions that mutate the world through the admin path (`detonate`, `atc_scramble`, `spawn`) actually run
  in a report instead of piling up unexecuted — a report of an admin-effect mission now matches a live
  run. The sim thread's existing drain was refactored onto the same method.
- **ci/docs**: `demo-videos` workflow + release-runbook step (#918). A `workflow_dispatch`-only CI job
  (`.github/workflows/demo-videos.yml`) records the demo set headless on lavapipe and optionally uploads
  the mp4s to a GitHub Release (`release_tag` input) — never a required PR gate (software rendering is
  slow; gate videos deserve a human watch). The release runbook and every phase's acceptance checklist
  in `docs/roadmap.md` gain a "record + review + attach the phase demo videos" step; `docs/roadmap.md`
  and `docs/demo-recording.md` document the flow. The only always-on CI addition remains
  `validate-mission missions/demos/*.yaml` (no GPU) — "CI never requires a GPU" is preserved.
- **tools/mission**: `record_demo.py` driver + the v0.4.0 demo mission set (#917). `missions/demos/`
  adds five zero-content-pack scripted demos with `cameras:` shots — `demo-dogfight`, `demo-sam-strike`,
  `demo-formation-tour`, `demo-sensors-intercept`, and `demo-gallery-flyover` — plus a `demos.json`
  manifest. `tools/record_demo/record_demo.py` orchestrates each: launch `fl-server --time-rate`, connect
  a headless observer recorder, and `ffprobe` the output as a smoke check (`--all` records the set,
  `--headless` sets the lavapipe env, `--png` skips ffmpeg). CI validates every demo mission on each PR
  (`validate-mission missions/demos/*.yaml`, no GPU) and unit-tests the driver's pure helpers. Reference:
  `docs/demo-recording.md`.
- **game**: cinematic recorder client mode (#916). New `--record <out.mp4>` (+ `--record-fps`,
  `--record-res`, `--record-png-dir`, `--shot-track`, `--exit-on-mission-end`, `--record-max-sec`,
  `--record-max-dup`) drives the camera from a mission's `cameras:` shots via `ShotDirector` — resolving
  entity-relative shots through the #914 roster — and pipes rendered frames to an external ffmpeg H.264
  process (`VideoEncoderPipe`, never linked; PNG-sequence fallback via stb_image_write). The pure
  `RecordScheduler` maps the 60 Hz snapshot stream to fixed-fps output boundaries and counts duplicated
  frames; exceeding `--record-max-dup` (or any encoder error) exits non-zero so bad video is never
  silently shipped. Pairs with `--headless` + `fl-server --time-rate` for no-display, no-GPU recording.
- **renderer/game**: swapchain-free headless init + `--headless` client mode (#913). `VkRenderer` gains
  `initHeadless(w, h)` — a WSI-free path that renders into owned present-target images (no surface, no
  swapchain, no present) so, paired with a software Vulkan ICD (lavapipe), the client renders with **no
  display and no GPU**. The game's `--headless` flag swaps in a no-op window/display, skips input/cursor
  backends, and drives the renderer's headless path; it rides the existing observer/auto no-menu flow.
  Verified end-to-end on lavapipe: `DISPLAY= VK_ICD_FILENAMES=…lvp_icd… fighters-legacy --headless
  --mission builtin:sandbox --auto --screenshot out.png` produces a correct 1280×720 PNG.
- **renderer**: `IRenderer` frame-capture sink + `VkRenderer` swapchain readback (#912). New
  `IRenderer::setCaptureSink(std::function<void(const CaptureFrame&)>)` (non-pure, false default — mocks
  unchanged) delivers every rendered frame's tightly-packed RGBA8 pixels to a sink at the end of
  `endFrame()`. `VkRenderer` implements it via a shared `vkCmdCopyImageToBuffer` readback (the existing
  `--screenshot` PNG path was refactored onto the same helper), swizzling BGRA swapchains to RGBA. The
  readback is synchronous — correctness over a zero-stall ring, since the recorder runs offline at a
  reduced time-rate. The GPU-independent swizzle core (`captureSwizzleToRgba`) is pure and unit-tested.
- **netcode**: `MsgMissionRoster` (`0x1B`) entity→mission-object-id table (#914). Sent reliably after
  `MsgConnectAck` (plus single-record deltas as player slots bind), it maps each spawned mission
  object's entity idx/gen to its mission object id, so the cinematic recorder can resolve an
  entity-relative camera shot's `target`/`look_at` ("orbit bandit1") to a live network entity.
  Self-describing concatenated records modelled on `MsgFactionDef`; additive, `kProtocolVersion`
  unchanged. **Mission-end** for the recorder's `--exit-on-mission-end` reuses the existing
  `MsgMissionOutcome` (`0x18`, #584) — which already broadcasts the terminal outcome from the same
  `MissionRuntime::setOnEnd` hook — rather than adding a redundant `CombatEventType::MissionEnd` with
  no distinct payload (the "clients never learn mission end" gap the epic cited was closed by #584).
- **server**: `--time-rate` flag + `TimeRate::Quarter`/`Eighth` (#915). fl-server can now run its sim at
  a reduced wall-clock rate (`--time-rate quarter|eighth|…`) applied after startup. The sim step stays
  1/60 s — content is byte-identical; ticks simply arrive slower in real time, so a slow (software-
  rendered, lavapipe) recording client receives every snapshot and never misses a capture boundary.
  The compensation lever behind the cinematic recorder's "one frame per boundary, fail loud on drops"
  timing model.
- **mission**: `ShotDirector` camera-pose evaluator (#911). A pure GLM+stdlib evaluator (engine-mission,
  no HAL) that turns a mission's `cameras:` shots (#910) + a live entity-pose callback into a camera
  pose at a given sim time. Implements all four shot types — `static`, `orbit` (radius/height/period,
  negative period = clockwise), `chase` (body-frame offset + deterministic exponential eye smoothing
  keyed on the fixed capture step), and `move` (linear or Catmull-Rom keyframe interpolation). Hard
  cuts between shots, gaps hold the previous shot's final pose, before-first holds the first shot's
  start pose, and a dead/unresolvable target holds the last valid pose (never a snap-to-origin). FOV
  is clamped [20, 120]. Fully unit-tested in `test_shot_director`.
- **mission**: optional `cameras:` shot schema in mission YAML (#910). A mission may now carry a
  presentation-only `cameras.shots` list — `static`, `orbit`, `chase`, and `move` shots with a fixed
  or entity-relative look target — parsed by the single schema owner (`parseMission`), so
  `validate-mission` covers it for free. Times are sim-seconds from mission start; shots are
  validated sorted + non-overlapping, `fov` defaults to 60° and is range-checked [20, 120], and
  `target`/`look_at` entity ids are cross-checked against `objects` (skipped for a cameras-only
  `--shot-track` sidecar). The server parses and ignores the block; the recording client's
  `ShotDirector` consumes it. Foundation for the cinematic demo-recording pipeline (epic #909).

- **game**: ground crew and base operations (#55). New "base refuel|rearm|repair" radio verbs: server-authoritative ground-crew services honoured only for an aircraft shut down (stopped, on the ground) at a base — within a few km of an airport (fl-server wires `AirportRegistry`; the zero-pack sandbox services anywhere) or on a carrier deck. Refuel tops the tanks and clears leaks; rearm rebuilds the default loadout with the same builder a spawn uses (#812) and refills countermeasures; repair restores hull, subsystem pools, damage penalties, and engine-fail flags. The crew chief answers over the same radio/subtitle path as ATC, including refusals with reasons. Client side: the comms menu (T) gains a Ground crew page; landing detection on the ownship finally gives `PilotLogbook::recordLanding` (#674) a producer (touchdown scored from sink rate); and after shutting down on the ramp the Chase camera blends into a slow ground-crew orbit around the parked aircraft, clearing on the takeoff roll, a camera-mode change, or a 60 s timeout. Everything degrades gracefully with no pack assets — the menu, radio text and scene need none.
- **engine**: carrier entity system (#38, consuming the #699 `accepts_landings` seam). A carrier is an ordinary moving entity: a new `type = "vessel"` flight model (`VesselForceModel` — keel propulsion, quadratic water drag meeting the declared top speed, rate-commanded rudder with steerage way, sea-level floor instead of the seabed) steered by any AI behavior, plus a `[deck]` block on the entity def (footprint, catapult stroke, arrest-wire zone, ship-local metres). The deck plane composes into the ground floor under any aircraft over it — landing, parking, and takeoff on a MOVING ship reuse the runway ground-handling path — parked aircraft are carried with the ship, and collision exempts aircraft at deck level (the landing path is not a mid-air). Catapult: stopped on the stroke at military power → hooked up and shot to `cat_end_speed_mps` (honouring the aircraft's `[carrier] cat_min_m_s`), with a haptic release. Arrest: a touchdown in the wire zone at trap speed catches a wire and is dragged to a stop ("Paddles: good trap!" + trigger rumble); faster is a bolter. An LSO calls glideslope/lineup/speed inside 3 nm on approach via the radio path. The deck footprint tail-appends to `MsgEntityTypeDef` (336 → 348 bytes) so client prediction composes the same moving floor the server does. `builtin:carrier` + `BuiltinCarrierVesselModel` make the whole launch/recovery cycle provable zero-pack; `validate-flight-model` gains the vessel reduced schema and the entity parser validates `[deck]`.
- **ai**: coordinated drone-swarm behaviors (#353). New `SwarmController` implements classic boids — separation / alignment / cohesion over the shared `SpatialIndex` (exact 3D-filtered, per-member neighbor cap so a large cloud stays O(members × cap), never O(N²)) — plus loose flock speed matching and a migration goal: a fixed world point (`--ai swarm <cx> <cy> <cz>`) or a moving anchor entity (`--ai swarm_follow <entityIdx>`). Flockmates are same-type, same-faction entities; each member is independent, so losing members degrades the flock rather than breaking it, and a null spatial index (tests/headless) degrades to an entity scan.
- **flight**: fixed-wing drone profiles via `[drone_limits]` (#351). An optional block on any fixed-wing flight model describing the onboard autopilot's command envelope — distinct from `[aero.limits]` (what the airframe survives) and `has_fbw` (a manned FLCS): `max_g` runs the same AoA-limiting loop as FBW on a non-FBW airframe (the tighter of autopilot and structural limits binds), `max_bank_deg` shapes the aileron command with the same never-more-than-commanded discipline, and the `min/max_airspeed_mps` band shapes the throttle (overspeed sheds power, underspeed firewalls it, one sensor-tick of honest latency). Every field optional, 0 = gate off, block absent = bit-identical. `validate-flight-model` range-checks the block, cross-checks the airspeed band, and warns when the autopilot g-limit can never bind.
- **flight**: helicopter rotor-disc force model (#350). `type = "helicopter"` flight models fly a new `HelicopterForceModel` behind the `IForceModel` seam: collective drives density-scaled disc thrust with ground effect (fed by a new `AeroInputs::agl_m` the integrator supplies from the terrain query) and effective translational lift; cyclic tilts the disc with rotor-follow rate damping; pedals command the tail rotor against an optional main-rotor torque reaction; blade flapping appears as the flapback speed-stability moment; and an unpowered disc autorotates — axial momentum drag through the disc caps an engine-out sink rate at a survivable figure instead of free fall. Authored via a reduced `[helicopter]` schema (turboshaft `[engine]` fuel flows required, no thrust decks), validated by `validate-flight-model` (cannot-hover error, disc-loading plausibility band); the generated manual's hover chart covers helicopters too.
- **flight**: multirotor force model (#349). `type = "multirotor"` flight models fly a new `MultirotorForceModel` behind the `IForceModel` seam: density-scaled per-rotor thrust along body-up (hover ceiling emerges from the atmosphere, no tables), rate-mode attitude mixing with the flight-control inner loop in the model, differential-torque yaw, flat-plate frame drag, per-rotor engine-out asymmetry, and endurance through the normal fuel path (`flight_time_min` becomes a constant drain; a dead battery is a #308 flameout). Authored via a reduced `[multirotor]` schema — no CL tables or turbine decks — validated by `validate-flight-model` (including a cannot-hover thrust-to-weight error). Force-model selection now routes through one shared seam (`flight/ForceModelSelect.h`) used by BOTH the server and client prediction, closing the latent parity gap where the client always replayed the fixed-wing model; `fl::trim()` declines non-fixed-wing models honestly, and the generated manual derives a hover chart (thrust-to-weight, hover throttle, hover ceiling, endurance) for rotorcraft instead of the level-flight trim table.
- **flight**: engine failure dynamics — flameout and compressor stall (#308). The integrator now raises the two transient `kEngineFail*` bits that were defined, carried on the wire, and consumed by haptics but never set by anything: `kEngineFlameout` on fuel starvation (an empty tank used to change the mass and nothing else) or above an optional `[engine] flameout_alt_km` combustion ceiling, cleared by a windmill relight (below the ceiling with airspeed >= `relight_min_mps`); and `kEngineCompStall` from the opt-in `[engine] compressor_stall` surge model (deep past stall alpha at high commanded power, transient total thrust loss with a ~2 s recovery). Bit ownership is split — the damage path latches Generic/Left/Right/Center, the integrator owns the transient bits — so the two writers cannot fight, and both flags are derived deterministically from flight state so client prediction replays them with nothing new on the wire. All fields optional; a model that omits them is bit-identical except for the fuel-starvation fix. `validate-flight-model` range-checks the new fields and cross-checks the flameout ceiling against the AB envelope.
- **ci**: generalized `auto-close-epics` workflow, backported from `mkzsystems/pm-framework` — closes a parent epic when its last sub-issue closes; the predicate matches the Epic issue type or the `epic` label.
- **server**: fl-server ATC wiring, `[atc]` config, and the end-to-end sandbox flow (#706, closing the ATC epic #673). fl-server builds an `AtcService` from the airport registry at startup (enabled by default via `[atc] enabled`) and wires the scramble spawn path: a scramble spawns aircraft hold-short of the runway — oriented down the runway heading — registers each a departure composition (hold-short → cleared-takeoff → climb-out → loiter), and files a takeoff request, so `atc_scramble builtin:airfield <type> 2` launches two AI that depart the same runway strictly sequenced. `[atc]` gains `enabled` (default true) and `scramble_entity_type` (default `builtin:debug-entity`); with `enabled = false` no facilities exist and radio commands answer "no ATC available". Documented in `docs/fl-server-config.md`, with a decision record in `docs/architecture.md`. This closes epic #673 (ground handling #700 → controllers #701 → engine-atc #702 → radio channel #703 → comms menu #704 → Lua/admin #705 → this integration).
- **game**: Comms menu, subtitle overlay, and voice-callout wiring (#704, part of the ATC epic #673) — the player-facing half of ATC, and #673 criterion 2. A new non-modal `CommsMenu` (opened with **T**; digits pick items, Escape backs out) whose ATC page emits `MsgRadioCommand` verbs (request takeoff/landing/inbound/cancel) — the shared radio-menu core the #610 wingman page reserves a root slot for; like the wingman menu it keeps the aircraft flying while open. Server radio transmissions render through the now-instantiated `VoiceCalloutManager` (a new `playText` overload takes the server's already-resolved line) plus a new `SubtitleOverlay` that draws the `SubtitleQueue` as a bottom-centre HUD overlay layer — finishing the dormant subtitle pipeline through the working `HudElement` path (Phase 6 IGui will replace it). Degrades to a text-only subtitle when a content pack provides no `radio/` audio, per docs/ai-architecture.md; `VoiceCalloutManager::init` now tolerates a null audio device. `test_comms_menu` covers page navigation, command emission, and close behaviour.
- **script**: `atc.*` Lua module and ATC admin commands (#705, part of the ATC epic #673) — the scripted/mission-facing surface of the ATC service, and #673 criterion 3. `LuaController` gains an optional `AtcService*`; a new sandboxed `atc.*` module exposes `atc.clearance()`, `atc.request_takeoff([id])`, `atc.request_landing([id])`, `atc.inbound([id])` (own-entity self-service), `atc.scramble(airport, type, count)`, and `atc.hold(airport, on)` — all nil/false-safe when no service is wired, and thread-safe (the service takes its own lock) so scripts may call them from the AI pass. fl-server admin commands `atc_status [airport]` (synchronous read of queues/occupancy), `atc_scramble <airport> <type> [count]`, and `atc_hold <airport> <on|off>` give operators (and the MCP/agent-reachable path) the same reach. Documented in `docs/sandbox.md` and `docs/modding/ai.md`.
- **network**: Player radio channel — `MsgRadioCommand` (`0x19`, client→server) and `MsgRadioTransmission` (`0x1A`, server→client), one generic radio channel shared by ATC now and reusable later (#703, part of the ATC epic #673). The command is verb-routed like the admin channel (`atc request_takeoff|request_landing|inbound|cancel [facility]`) — never a direct state mutation; `WorldBroadcaster` resolves the requesting peer's aircraft, rate-limits per peer, dispatches the verb to the ATC service, and replies with an acknowledgement, while facility-specific clearances flow from the 1 Hz ATC tick. Transmissions carry a speaker, a localizable server-rendered line, a stable `voiceKey` (TTS / content-pack OGG, empty = subtitle only), and a subtitle dwell; they unicast to the addressed pilot or broadcast an AI flight's clearance. The client prints `[radio] speaker: text` and exposes a `radioCallback` for the comms-menu subtitle/voice pipeline (#704). `kProtocolVersion` unchanged (additive ids; `0x0D`/`0x0E` were already taken by the wingman channel, so the radio channel took the next free ENet ids).
- **engine**: ATC service core — a new `engine-atc` library (`namespace fl::atc`) providing the server-authoritative, deterministic air-traffic-control FSM (#702, part of the ATC epic #673; no model involvement anywhere, per docs/ai-architecture.md). `AtcFacility` binds a runway to a departure FIFO + a distance-sequenced arrival queue + single-runway occupancy + per-flight `ClearanceState`, granting takeoff when the runway is free and no arrival is on short final, landing to the nearest arrival, and go-arounds when the runway is occupied at short final; players participate identically (occupancy is read from `EntityState`). `AtcService` builds facilities lazily from the `AirportRegistry` (an 80k-airport registry costs nothing until traffic appears), exposes a thread-safe sim API (`requestTakeoff`/`requestLanding`/`declareInbound`/`holdDepartures`/`scramble`/`clearanceState`/`drainTransmissions`), and ticks at 1 Hz in `WorldBroadcaster`'s serial Maintenance phase (behind `setAtcService`, with an injected transmission sink for the #703 wire message). `AtcBehaviors.h` composes the takeoff/landing controllers into `StateMachineController` departure/arrival behaviours. Every radio line is an enumerable `AtcPhrase` with a stable voice key so the AI-voice ATC epic (#591/#936) binds TTS 1:1. Also fixes a latent orphan in `WorldBroadcaster`: a controller whose entity was destroyed outside `onDisconnect` is now reaped during the gather pass instead of lingering until pool-index reuse.
- **ai**: Takeoff, landing, and hold-short controllers (#701, part of the ATC epic #673) — the ground-and-pattern flight primitives the ATC service sequences. `TakeoffController` self-drives LineUp → Roll → Rotate (at Vr) → Climb → Done off its own ground speed and AGL, tracking the runway heading and rotating/climbing out to a target AGL; `LandingController` flies Final (extended-centreline + glidepath tracking, throttle-for-speed) → Flare → Rollout (full wheel brakes + nosewheel steering from #700) → Done; `HoldShortController` sits idle for the parking hold to pin. Two new `StateMachineController` conditions, `AboveAltitude` and `GroundSpeedBelow`, gate the departure/arrival compositions. All are pure functions of the observed `EntityState` (flat-runway assumption, no terrain query) and follow the existing `IEntityController::sample()` pattern.
- **flight**: Ground handling — wheel brakes, rolling resistance, and nosewheel steering (#700, part of the ATC epic #673). `ControlInput::wheelBrake` (0–1) is applied by `FlightIntegrator` only in ground contact: a baseline ~0.02 g rolling resistance always decays a rollout, and full brake adds up to ~0.35 g on top, so a lander can actually stop on the runway instead of coasting off the end. Rudder now steers the nosewheel, converting to a yaw rate whose authority is full at taxi speed and fades to nil by ~50 m/s (the aero rudder owns yaw on the takeoff roll). Wire: `MsgClientInput` buttons bit 6 = wheel brake (`kInputButtonWheelBrake`, additive — no protocol bump); the game client holds the brakes with **B**. Parked statics and the takeoff roll are unchanged; this is load-bearing for the AI landing/rollout flow.
- **server**: Runnable campaign mode driving the dynamic-campaign engine (Epic #584) — closes the loop so the #635 campaign engine actually *runs*. A new `CampaignRunner` (`engine/campaign/CampaignRunner.h`) ties `CampaignEngine` to mission content + persistence: `nextMissionYaml` resolves the next sortie into a concrete mission (a story file verbatim, or a dynamic template materialized via `materializeMissionTemplate`), `recordOutcome` advances the campaign, and `save`/`restore` round-trip the state. fl-server gains `--campaign <file>`: it parses the campaign, restores any saved state (`cache/campaign_<name>.flsave`), flies the current sortie, and on the objective outcome records it + persists — so each run advances the persistent war (story injects at trigger, the theater/frontline state advances after objective completion, dynamic sorties generate from it) and a restart continues from the saved state. Verified end-to-end through the `--mission-report` harness: run 1 flies the campaign_start story and unlocks the theater, run 2 restores and flies a materialized dynamic strike. (Frontline PNG raster decoding is content-gated and left as a host seam; the campaign progresses via the story/dynamic/attrition/`set_frontline` state.) Part of #584.
- **game**: Real mission outcome at the debrief (Epic #584) — the debrief no longer hardcodes success. When the objective evaluator drives the mission to Complete/Failed, fl-server broadcasts a new reliable `MsgMissionOutcome` (0x18) carrying the result + elapsed/triggers; the client stores it and the debrief reports the true success/failure (and the pilot logbook records the mission as failed when it failed). A session with no mission (Free Flight) sends none, so it still reads as success. Part of #584.
- **docs**: Lua scripting API reference for the Phase-4 bindings (Epic #584) — `docs/modding/ai.md`'s "Coming in Phase 4" stub is replaced with the real reference now that the bindings ship: the coroutine control-flow model (`ai_main` + `coroutine.yield`, #412), the full `world.*` module table (spawn/despawn/set_relationship/set_music_state/mission_success/mission_failure/get_elapsed_time/on_trigger/timer, #413) with a worked ambush example, and the haptic bindings (`rumble`/`rumble_triggers`/`stop_rumble`, #128) with their sandbox guards. Cross-links to `docs/haptics.md` for the wire path. Closes #185
- **game**: Ejection, pilot survival, and outcome consequences (Epic #584) — the pilot is now separate from the airframe, so the campaign and stats layers can express MIA/KIA/rescued instead of a binary alive/dead. A pure, unit-tested ejection model (`engine/entity/Ejection.h`) decides two things deterministically: `ejectionSurvivable(envelope)` — a zero-zero seat saves a pilot on the deck in level flight but not while diving into the ground below the chute-deploy altitude or above the windblast speed limit — and `pilotOutcome(survived, territory)` — a dead seat is KIA, and a survivor's landing zone maps to Rescued (friendly) / Captured (hostile) / MIA (no-man's-land). A new `Eject` `InputAction` (default **End**) sets a new `MsgClientInput` eject bit; `WorldBroadcaster::ejectPilot` edge-detects it (a held key is one ejection), evaluates the seat envelope from the aircraft's live flight state, spawns a **replicating parachute** (`builtin:parachute`, an Effect entity that rides the normal snapshot path to every client), and destroys the airframe. KIA and a survived ejection are deliberately distinct outcomes so career stakes and campaign branches can turn on them. **AI pilots auto-eject** when critically hit (a `WorldBroadcaster::setAiAutoEject` opt-in, on in fl-server) — a decimatable AI airframe below the critical HP fraction punches out once, spawning the same replicating chute, so AI aircraft eject visibly. **Territory resolution** is wired through a `setTerritoryQuery` seam: a campaign points it at the frontline (`Frontline::territoryAtWorld` maps the landing cell's control + the pilot's side to Friendly/Hostile/Neutral) so a survivor becomes Rescued/Captured; a plain server leaves it unset and resolves to MIA. Closes #672
- **game**: Persistent pilot logbook — kill tallies, weapon accuracy, career stats (Epic #584) — the career record that gives a debrief meaning. `PilotProfile` gains a `PilotLogbook` (`engine/config/PilotLogbook.h`): per-target-class kill tallies indexed by the **entity taxonomy** (`ObjectCategory` ordinal — one taxonomy, many consumers, not a parallel list), per-weapon-category accuracy accumulators (shots/hits/kills for A/A gun, A/A missile, ground-attack, naval), and career counters (missions flown/failed, ejections, best/last landing score). It persists under `[pilot.logbook]` in `user.toml` and survives restart. The debrief writes the same deltas it shows: the client now classifies each kill it scored by the victim's entity category (from the kill feed + type registry) into `SessionCombatStats::killsByClass`, and the debrief accrues those per-class into the logbook so the logbook's total matches the debrief's kill delta exactly (unclassifiable kills still count). Rank/medal evaluation and the campaign (#635) read only the logbook + campaign state, keeping it deterministic and testable. Closes #674
- **mission**: Dynamic campaign engine — theater graph, frontline state, mission injection (Epic #584) — the deterministic system of record for a persistent-war campaign, the machinery the campaign director (#590) will later drive. A new `engine-campaign` library delivers all three decomposed parts of #635: (1) the **frontline raster** (`Frontline`) — an 8-bit grayscale control field over a theater's geographic bounds with pixel↔geo↔world mapping and side-control fractions, holding decoded pixels only so engine-campaign needs no image library; (2) the **campaign schema parser** (`parseCampaign`, the single owner `validate-campaign` #784 will delegate to) and the **theater-graph state machine** (`CampaignEngine`) — story missions inject at their triggers (`campaign_start` / `after_sorties:N` / `frontline_reaches:<tag>` / prior mission's `next:`), a `locks_dynamic` story freezes the dynamic war while pending, dynamic sorties are selected by a **seeded weighted draw** (replay-deterministic) filtered by each template's `requires` tag, and completing a story advances the frontline (`set_frontline` replace), unlocks theaters, and applies attrition to the enemy order of battle; (3) **save/restore** round-trips the mutable runtime state (sorties flown, completed/pending story, unlocked theaters, per-theater frontline + ground units). A generated dynamic sortie also **materializes into a concrete mission file**: the engine resolves each template fill against the live frontline / ground units (target-area, ingress, opfor, player-flight size) into `NextMission::fills`, and `materializeMissionTemplate` strips the template's `template:` header and substitutes its `${...}` placeholders, producing a plain mission YAML that round-trips through the ordinary mission parser (so a dynamic sortie is saveable, replayable, and validatable like any authored mission). Frontline PNG decoding is injected via a host `FrontlineLoader`, keeping the whole engine unit-testable with synthetic rasters. Zero AI/LLM involvement — the required Phase 4 acceptance behavior. Closes #635
- **engine**: Lua rumble API for mod authors (Epic #584) — Lua AI/mission scripts can trigger haptic feedback via three bare globals: `rumble(low_freq, high_freq, duration_ms)`, `rumble_triggers(left, right, duration_ms)`, and `stop_rumble()`. No gamepad id is exposed — the call targets "the current player's gamepad": a script runs server-side, so the request is routed through the `WorldApi` seam, fl-server broadcasts a reliable `MsgHaptic` (0x17), and each client plays it on its own local gamepad (id 0) via `IInput`. Sandbox guards live in the engine binding — intensities clamp to `[0, 1]` and one request is capped at 5000 ms, so an untrusted mod cannot latch rumble on (`stop_rumble` is always available). A headless server / null-input mock makes the whole path a clean no-op. Documented in `docs/haptics.md`. Closes #128
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

### Fixed

- **renderer**: windowed frame capture read back all black on real drivers. The per-frame capture sink and the screenshot readback (#912/#909) copied the swapchain image *after* `vkQueuePresentKHR`, whose contents are undefined per the Vulkan spec until the image is re-acquired — a real driver (NVIDIA) returns black even though the on-screen frame is correct, while software/headless rendering happened to retain the content so it went unnoticed (CI records headless). Move the readback ahead of present, while the app still owns the rendered image (windowed: already in `PRESENT_SRC_KHR`; headless: its owned `TRANSFER_SRC_OPTIMAL` image); the layout is restored so the frame is still present-ready, and the non-capturing render path is unchanged. Verified windowed on an RTX 5080. Part of #909 (PR #990)

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
