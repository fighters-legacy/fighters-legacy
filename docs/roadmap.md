# Roadmap

Development is tracked through [GitHub milestones](https://github.com/fighters-legacy/fighters-legacy/milestones).
Each phase has a milestone with individual issues for every workstream task.

## Schedule

Phases are sequentially gated. Week numbers from the original plan are removed — they
drifted from reality during Phase 2. Ordering constraints are listed instead.

| Phase | Name | Gate |
|---|---|---|
| 1 — Engine Foundation ✓ | HAL, content system, CI/CD | — |
| 2 — Modern-Particles Engine ✓ | Game loop, flight model, networking, renderer, spherical Earth | Phase 1 complete |
| 3 — Engine Systems | Spatial partitioning, AI framework, interest management, bindings, quality settings, **scaling seams** (transport replacement, sim job system, wire quantization, load harness) | Phase 2 complete |
| 4 — Content & Gameplay | fl-base-pack content, radar/weapons + sensor framework, AI system, missions, **MP gameplay framework**, advanced vehicle models | Phase 3 complete + fl-base-pack substantially ready |
| 5 — Multiplayer at Scale & Live Services | Server-side identity/auth, anti-cheat, persistence, ops/observability, k8s/OpenShift operator | Phase 4 complete |
| 6 — UI Layer & Tooling | IGui HAL + Dear ImGui, in-game mission editor, welcome screen | Phase 5 complete |
| 7 — Platform Release | macOS/Linux/Windows packages, Flathub, fl-server container, crash reporting | Phase 6 complete |
| 8 — OpenGL & Alternative Renderers | OpenGL 4.1 Core, headless/software renderer for CI, voice chat | Phase 7 complete |
| 9 — Modding Platform | GPG verification, subprocess isolation, in-game mod browser, community content distribution | Phase 8 complete |

> **2026-06-28 re-target to 128+ multiplayer.** A new **Phase 5 — Multiplayer at Scale & Live
> Services** was inserted and former Phases 5–8 renumbered to 6–9 (release slips one phase —
> a conscious choice). Scaling seams were folded into Phases 3–4. See the
> [decision record](architecture.md#decision-records) and the cross-cutting initiative below.

---

## Cross-Cutting Initiative: Multiplayer at Scale & Live Services

The 128+ target is delivered by twelve epics that thread across Phases 3–5. They are sequenced
by dependency, not by phase boundary:

| Epic | Theme | Phase |
|---|---|---|
| A | Server simulation scalability (data-parallel job system, tick budget) | 3→4 |
| B | Network bandwidth & snapshot scaling (quantization ✓ #515, 3D interest ✓ #402, priority/budget ✓ #516, acked baselines ✓ #517, congestion #518) | 3→4 |
| I | Load-testing / bot-swarm harness + 128-client scale gate | 3→4 |
| L | Network transport replacement (enet6 → GameNetworkingSockets behind `INetwork`) | 3→4 (transport optimization) |
| E | Multiplayer gameplay framework (game modes, teams, scoring, reconnect, spectator) | 4 |
| F | Combat sensors, datalink & EW (radar modes, IFF, shared track picture) | 4 |
| J | Voice comms (positional + team; moved earlier from Phase 7) | 4/6 |
| H | Persistence layer (`IPersistence`: accounts, stats, bans, world state) | 5 |
| C | Player identity, accounts & authentication (pluggable, offline-verifiable tokens) | 5 |
| D | Anti-cheat & competitive integrity (live validation + offline `fl-review`) | 5 |
| G | Server ops & observability (metrics, Grafana, admin web interface) | 5 |
| K | Cluster orchestration: k8s/OpenShift operator (Agones-native) | 5 |

**Dependency order (re-prioritised 2026-06-28 from reference-env load data, [#505](https://github.com/fighters-legacy/fighters-legacy/issues/505)):**
**A and B lead** — the empirical 8-core ceiling is gated by the single-threaded sim (A) and
per-client snapshot bandwidth (B), *not* the transport, and B's quantization is transport-agnostic.
I (the harness) validates them. **L is no longer foundational/blocking** — enet6 is not the
bottleneck in the 96–256 range; L is now a later transport optimisation (encryption, congestion
control, connection-count headroom) that pairs with Epic C auth. Then (H → C → D) with G alongside
H/C → K last. E, F, and J run in Phase 4 independent of the live-services chain.

**New repos (Go):** `fl-account` (identity), `fl-review` (offline anti-cheat), `fl-operator`
(k8s/OpenShift operator + Helm chart). The engine/game/server stay C++.

**Hosting model:** self-host only. The project ships the software; communities run their own
servers and identity. No first-party hosted infrastructure, no PII/GDPR liability for the
project. The path to optional official infrastructure later is kept open by globally-unique
account IDs + a realm/scope field in persistence (additive, not a rewrite).

**Scale acceptance (Epic I gate):** 128 players @ 60 Hz with sim tick ≤ 16.6 ms p99 on a
reference 8-core / 16 GB instance; sustained ≤ ~150 KB/s/client downstream after quantization +
budgeting; soak-stable for 2 h. Phase 4 multiplayer acceptance depends on Epics A/B/I proving
this in addition to its existing criteria.

---

## Critical Path

1. **Spatial partitioning (#360) → interest management (#346) → multiplayer at scale (Phase 4)**
   Broadphase index enables range queries needed for interest management; both must be in
   before Phase 4 multiplayer acceptance testing with real clients.

0. **Load harness (Epic I ✓) → sim parallelism (Epic A) + wire quantization (Epic B) →
   128-player acceptance.** The bot-swarm harness ([#519](https://github.com/fighters-legacy/fighters-legacy/issues/519))
   measured the 8-core ceiling ([#505](https://github.com/fighters-legacy/fighters-legacy/issues/505)):
   it is gated by the single-threaded sim (A) and per-client snapshot bandwidth (B), **not** the
   transport. A and B proceed on the current `enet6` (B's quantization is transport-agnostic).
   Transport replacement (Epic L) is decoupled to a later optimisation (encryption/congestion/
   connection-count headroom), to be re-evaluated once A/B raise the sim ceiling.
   **Epic A progress:** the design spike ([#510](https://github.com/fighters-legacy/fighters-legacy/issues/510))
   chose a data-parallel single tick (not spatial sharding) and the `engine-job` worker pool +
   parallel AI/integrate passes landed ([#511](https://github.com/fighters-legacy/fighters-legacy/issues/511));
   snapshot-assembly parallelism (#512) and graceful overrun handling (#514) remain. See
   [server-job-system-design.md](server-job-system-design.md).

2. **LuaSandbox wired (#359 ✓) → fl-base-pack AI scripts → AI System (#33)**
   fl-base-pack Lua behaviour scripts can now target the `compute_control` API shipped in #359.

3. **Server-side AI framework (#352) → AI System (#33) → radar/weapons AI (#42)**
   Flight controller framework is the substrate; the full AI system and radar/weapons AI
   build on top of it in Phase 4.

4. **fl-base-pack #1–#6 (aircraft, terrain, missions, audio, AI) → Phase 4 acceptance testing**
   fl-base-pack content work can start in parallel with Phase 3. Phase 4 acceptance is gated
   on fl-base-pack being substantially complete.

5. **IGui HAL (#156) → in-game mission editor (#49), subtitle rendering (#165), crash overlay (#236)**
   All Phase 6 features depend on the IGui interface being stable.

6. **Persistence (Epic H) → identity (Epic C) → anti-cheat (Epic D) + operator (Epic K)**
   Anti-cheat verdicts and per-account ranking key on identity, which keys on the persistence
   store; the operator packages and deploys the live-services tier last in Phase 5.

7. **Platform packaging (Phase 7) → OpenGL renderer (Phase 8)**
   OpenGL is an optional compatibility layer; it should not block the primary release.
   The headless/software renderer (Phase 8) enables GPU-free CI visual regression tests.

---

## Verification / Acceptance Criteria

### Phase 1 — Engine Foundation ✓

- All three CI jobs build clean (Windows/Linux/macOS).
- Vulkan validation layers: zero errors on triangle hello-world.
- MoltenVK smoke test passes on Apple Silicon.
- Engine boots cleanly with zero content packs installed; sandbox inspector reachable.

### Phase 2 — Modern-Particles Engine ✓

Phase 2 acceptance is the **standalone playable sandbox** — no content pack required.

- Game binary boots cleanly with zero content packs installed.
- Player reaches free-flight in the sandbox in under 30 seconds; no crash; no error modal.
- Builtin aircraft is flyable (pitch/roll/yaw respond, climbs/descends, throttle works).
- Builtin terrain renders via `TerrainStreamer` fallback; `heightAt()` returns valid elevations.
- HUD renders in minimal mode (altitude, airspeed, heading, throttle %).
- Main menu shows "Sandbox (no pack)" entry when no content pack is detected.
- World positions are double-precision throughout; no float precision errors at large scale.
- GPU particles render for explosion / smoke / fire.
- Authoritative fl-server + ENet client networking operational on all three platforms.
- Wire protocol documented (`docs/network-protocol.md`).
- enet6 backend active; fl-server binds on `::` dual-stack.
- Spherical-Earth world model functional; `CentralGravityField` and `TerrainStreamer` curvature correction active.
- CI green on all three platforms (debug, debug-msvc, macOS).

### Phase 3 — Engine Systems

Phase 3 acceptance is a **complete engine layer** — all features testable with zero content packs.

- Spatial partition queries functional; broadphase neighbor search passes unit tests.
- Snapshot interest management active; bandwidth under threshold with 20 simulated clients.
- LuaSandbox wired to `IEntityController`; a scripted sandbox entity responds correctly in tests.
- Server-side AI flight controller framework: at least one AI entity maintains altitude in sandbox.
- `FlightState::pos_world` is `double[3]`; all integrator math consistent with dvec3 world positions.
- Pilot profiles persist across sessions; stats updated at mission debrief.
- Advanced quality settings: shadow resolution, particle density, and AA mode selectable and saved to user.toml.
- Per-vertex spherical terrain mesh correction: no visible seams or skirts at altitude.
- libFuzzer harnesses in CI; zero crashes on seed corpus.
- Connection heartbeat/keepalive: ENet peer timeout behaves correctly under packet loss.
- Client-side prediction active for the player entity; inputs feel responsive on 100 ms RTT connections without visible snapping artifacts.
- `bindings.toml` loaded; per-axis HOTAS/gamepad mapping applied at startup.
- WeatherPreset::Snow and WeatherPreset::Blizzard functional (weather state machine + visual presets).
- NVG cockpit overlay toggles on/off in cockpit mode.
- Scaling seams landed: transport replacement (Epic L) selected behind `INetwork` and passing
  a transport scale-spike; load-test bot-swarm harness (Epic I) runs in CI; server tick-budget
  instrumentation (Epic A) reports per-phase timing; wire quantization (Epic B, #515 ✓) bit-packs
  the snapshot entity stream and 3D interest culling (#402 ✓) lands, with snapshot/`sizeof` tests
  updated.
- All three CI platforms green.

### Phase 4 — Content & Gameplay

Phase 4 acceptance requires a content pack (fl-base-pack) and is gated on Phase 3 completion.

- A fl-base-pack mission loads and runs to completion via `ModLoader`.
- Flight model stall speed + fuel burn match design spec for each fl-base-pack aircraft type.
- Radar lock, missile fire, and countermeasure sequence works per fl-base-pack weapon definitions.
- Progressive damage: light / heavy / critical thresholds produce correct visual + flight penalties.
- AI system: wingman follows player and responds to all six commands.
- Dynamic campaign: frontline advances after objective completion; story mission injects at trigger.
- Instant Action / Quick Play: reachable without manual mission YAML setup.
- Replay: mission records and plays back from cockpit and free-camera views.
- Multiplayer: two clients on fl-server complete a cooperative strike mission.
- MP gameplay framework (Epic E): a data-driven game mode (e.g. team deathmatch) runs a full
  match lifecycle (warmup → active → end → rotation) with team assignment, scoring, spectator,
  and drop-in/reconnect.
- Sensor framework (Epic F): radar search/track + IFF + a shared team track picture function
  against fl-base-pack content.
- Scale proven: Epics A/B/I demonstrate the 128-client tick + bandwidth gate (see the
  cross-cutting initiative) — a prerequisite for Phase 4 multiplayer acceptance.
- Helicopter and multirotor force models functional with appropriate fl-base-pack aircraft types.
- Per-engine failure simulation: L/R bits produce asymmetric thrust effects in cockpit.
- Afterburner envelope limits enforced per aircraft TOML definition.
- Lua scripting API documented (`docs/modding/ai.md`).
- CI green on all three platforms.

### Phase 5 — Multiplayer at Scale & Live Services

Phase 5 acceptance is the **live-services tier** that makes 128-player public/community servers
operable, identifiable, and cheat-resistant. Engine-layer scaling seams (transport, job system,
wire quantization, load harness) are validated earlier as Phase 3–4 gates.

- Transport replacement (Epic L) holds the Epic I scale gate: 128 clients @ 60 Hz, sim tick
  ≤ 16.6 ms p99 on the reference instance, soak-stable for 2 h.
- Quantized snapshot stream + priority/budget scheduling keep sustained downstream
  ≤ ~150 KB/s/client at 128 players.
- Server-side identity: a client authenticates via a pluggable `IIdentityProvider`
  (offline-verifiable signed token); guest play still allowed when the server permits it.
- Persistence (`IPersistence`): accounts, stats, and bans survive restart; file banlists import.
- Anti-cheat: live input validation rejects impossible states in-tick; the offline `fl-review`
  pipeline flags a seeded cheat corpus.
- Observability: `fl-server` exports Prometheus metrics; bundled Grafana dashboards render; the
  admin web interface performs kick/ban/config-reload against a running server.
- Operator: the k8s/OpenShift operator deploys a fleet, autoscales on population, and drains a
  live match gracefully on scale-down (reusing the shutdown countdown). Installs on OCP via OLM.
- All three CI platforms green; new Go repos green on their own CI lanes.

### Phase 6 — UI Layer & Tooling

- IGui HAL implemented with Dear ImGui backend on all three platforms.
- In-game mission editor: create, edit, and save a YAML mission on all three platforms.
- Round-trip: create a TOML aircraft + glTF mesh; load it in the engine without errors.
- Subtitle text rendering via IGui overlay functional.
- First-run welcome screen shown on initial launch.
- Crash report and mod-load failure overlay display correctly via IGui.

### Phase 7 — Platform Release

- macOS .app bundle signed and notarized; passes Gatekeeper on a clean machine.
- Linux Flatpak published to Flathub; AppImage available for direct download.
- Windows Inno Setup installer; statically-linked VCRT; no external DLL requirement.
- fl-server official container image published to GHCR.
- Crash reporting operational on all three platforms; reports reach the configured endpoint.
- All three platforms in CI green with release artifacts attached.

### Phase 8 — OpenGL & Alternative Renderers

- OpenGL 4.1 Core backend: all seven render passes functional on Mesa + ANGLE + Intel iGPU.
- Software/headless renderer: CI visual regression tests run without a physical GPU.
- In-game voice chat functional in multiplayer sessions on all three platforms.

### Phase 9 — Modding Platform

- GPG signature verification for community and maintainer content packs.
- SHA-256 manifest hash pinning enforced on update; tampered packs rejected.
- Subprocess isolation active for compiled content plugins on all three platforms.
- In-game mod browser: lists, installs, enables, and disables packs without restart.
- First-run fl-base-pack download from community index completes successfully.
- `validate-mod` passes on fl-base-pack.
- Modding documentation complete; content pack authoring guide published.
