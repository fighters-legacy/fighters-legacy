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
| 3 — Engine Systems ✓ | Spatial partitioning, AI framework, interest management, bindings, quality settings, **scaling seams** (transport replacement, sim job system, wire quantization, load harness) | Phase 2 complete |
| 4 — Content & Gameplay | fl-base-pack content, weapons/ballistics + sensor framework, mission & campaign runtime, **MP gameplay framework**, avionics/HUD, gameplay audio, advanced vehicle models, replay, agentic-AI substrate + campaign director (Epics M–O start), **consolidation program** (protocol hardening + interface consolidation) | Phase 3 complete + fl-base-pack substantially ready |
| 5 — Multiplayer at Scale & Live Services | Server-side identity/auth, anti-cheat, persistence, ops/observability, k8s/OpenShift operator, deployment artifacts (container image, systemd unit — moved from Phase 7) | Phase 4 complete |
| 6 — UI Layer & Tooling | IGui HAL + Dear ImGui, in-game mission editor, welcome screen | Phase 5 complete |
| 7 — Platform Release | macOS/Linux/Windows packages, Flathub, crash reporting | Phase 6 complete |
| 8 — Rendering & Alternative Backends | OpenGL 4.1 Core, headless/software renderer for CI, renderer advancement (temporal AA/upscaling, sky LUTs, IBL, AO quality) | Phase 7 complete |
| 9 — Modding Platform | GPG verification, subprocess isolation, in-game mod browser, community content distribution | Phase 8 complete |

> **2026-06-28 re-target to 128+ multiplayer.** A new **Phase 5 — Multiplayer at Scale & Live
> Services** was inserted and former Phases 5–8 renumbered to 6–9 (release slips one phase —
> a conscious choice). Scaling seams were folded into Phases 3–4. See the
> [decision record](developer/architecture.md#decision-records) and the cross-cutting initiative below.

> **Phase 3 gated at `v0.3.0` (2026-07-10); milestone closed 0 open / 143 closed.** Phase 4 is
> the active milestone. The scaling spine (Epics A/B/I/L) landed in Phase 3; the `v0.3.x` series
> ships interim patches while Phase 4 content is in development.

> **2026-07-30 strategic review.** A **consolidation program** lands inside Phase 4, before the
> gate, as new stages of the #1036 plan (Stages 1–3 shipped as v0.3.11–v0.3.13): protocol
> hardening and scale honesty (epic #1063) and interface consolidation and event unification
> (epic #1064), each stage shipping an interim `v0.3.14+` release. Phase 5 builds identity,
> persistence, anti-cheat and a cluster operator directly on the server seams, so the seams are
> cleaned first; the wire and admin surfaces also acquire external users at the gate, which makes
> afterwards strictly harder. The **gameplay audit (#1065) runs last**, after consolidation and
> the gating content, so it audits what actually ships. Deployment artifacts moved Phase 7 →
> Phase 5 (#160, #228, #1096): a self-host-only product ships its hosting artifacts before the
> fleet operator that consumes them. See the
> [decision record](developer/architecture.md#decision-records).

## Releases

Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html), pre-1.0.
**`v0.N.0` tags the Phase N gate** — cut when the phase's milestone closes with its
[acceptance criteria](#verification--acceptance-criteria) verified. Interim patch releases
(`v0.N.x`) ship fixes and increments while the next phase is in development, as
v0.2.1–v0.2.6 did during Phase 3 and v0.3.1–v0.3.13 did during Phase 4 — the consolidation
stages continue that series as `v0.3.14+`. **`v0.4.0` is the Phase 4 gate**, tracked by release
Task #729.

Each phase gate is tracked by a `release`-labeled Task in the phase's milestone. It sits at
milestone level rather than under an epic — a phase-gate release serves every epic in the
phase — and it is deliberately the last issue to close in its milestone, so a milestone
cannot close before its release ships.

**Runbook:** `./scripts/cut-release.sh vX.Y.Z` (release branch + CHANGELOG section generated from
commit subjects + CMake version bump) → read the generated section and check its scope →
the release PR merges → `./scripts/tag-release.sh vX.Y.Z` → the tag push runs `release.yml`
(Windows/Linux/macOS artifacts) → **wait for that workflow to complete** → verify the attached
artifacts → **hand-author the release notes and read the body back on that tag** →
**at a phase gate (`v0.N.0`), record + review the phase's demo videos and attach them to the
Release** → close the milestone. Full procedure and the defect each step prevents:
[project-management.md → *Cutting a release*](developer/project-management.md#cutting-a-release).

**Demo videos (phase gates, epic #909):** record the phase's demo set headless — locally via
`tools/record_demo/record_demo.py --all --headless`, or from the Actions tab via the opt-in
`demo-videos` workflow (`workflow_dispatch`, `release_tag` = the gate tag to upload the mp4s).
**Watch the footage** before attaching (software rendering is slow and never a required CI gate),
then attach it to the GitHub Release. The always-on gate is only `validate-mission
missions/demos/*.yaml` (no GPU). Reference: [demo-recording.md](developer/demo-recording.md).

**Phase-gate close checklist** (so the docs never lag a gate again, as they did between the
2026-07-10 Phase 3 close and this revision): tag `v0.N.0` → **record, review, and attach the
phase demo videos (#909)** → close the milestone → mark the Schedule-table row and the acceptance
heading ✓ in this file → run an epic re-home sweep (re-home any epic whose last open sub changed
phase) → prune landed items from the Critical Path → **file the incoming phase's execution-plan
Task** (the #1036 pattern: stages, interim releases, architecture decisions made up front) as the
new milestone's first issue.

**v1.0.0** follows the Phase 9 gate (v0.9.x is the last pre-1.0 series) and adds the
maintenance-mode policy to README/CONTRIBUTING.

---

## Cross-Cutting Initiative: Multiplayer at Scale & Live Services

The 128+ target is delivered by twelve epics that thread across Phases 3–5. They are sequenced
by dependency, not by phase boundary:

| Epic | Theme | Phase |
|---|---|---|
| A | Server simulation scalability (data-parallel job system, tick budget) | 3→4 |
| B | Network bandwidth & snapshot scaling (quantization ✓ #515, 3D interest ✓ #402, priority/budget ✓ #516, acked baselines ✓ #517, selective-ack precision ✓ #566, congestion ✓ #518) | 3→4 |
| I | Load-testing / bot-swarm harness + 128-client scale gate | 3→4 |
| L | Network transport replacement — **GameNetworkingSockets landed behind `INetwork`** ([#507](https://github.com/fighters-legacy/fighters-legacy/issues/507)/#508/#509; selected [#506](https://github.com/fighters-legacy/fighters-legacy/issues/506)), enet6 retained as the LAN/low-count backend via `createNetwork()`. GNS-native 128-client scale validation ([#649](https://github.com/fighters-legacy/fighters-legacy/issues/649)) ✓ and Windows/macOS GNS CI legs ([#653](https://github.com/fighters-legacy/fighters-legacy/issues/653)) ✓ landed. Remaining: the enet6-retirement decision ([#652](https://github.com/fighters-legacy/fighters-legacy/issues/652), now Phase 5 — see Deferred Levers). | 3→4 (transport optimization) |
| E | Multiplayer gameplay framework (game modes, teams, scoring, reconnect, spectator) | 4 |
| F | Combat sensors, datalink & EW (radar modes, IFF, shared track picture). Built on one shared sensor core ([#677](https://github.com/fighters-legacy/fighters-legacy/issues/677)) whose vocabulary is locked by the 2026-07-12 decision record — a single `SensorDef` schema and `Contact` track model serving avionics, AI detection ([#670](https://github.com/fighters-legacy/fighters-legacy/issues/670)) and missile seekers alike. | 4 |
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
control, connection-count headroom) that pairs with Epic C auth. Its selection spike
([#506](https://github.com/fighters-legacy/fighters-legacy/issues/506)) chose **GameNetworkingSockets**
(BSD-3) behind `INetwork`, with `enet6` retained as the LAN/low-count backend — see
[docs/developer/decisions/transport-selection.md](developer/decisions/transport-selection.md). Then (H → C → D) with G alongside
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

## Cross-Cutting Initiative: Dynamic World & Agentic AI

*(Added 2026-07-01 — see the [decision record](developer/architecture.md#decision-records). Epic letters
continue from A–L above. Full design: [docs/developer/ai-architecture.md](developer/ai-architecture.md).)*

A second initiative threads four epics across Phases 4–6: a **pluggable, local-first AI
runtime** that makes the world dynamic — an agentic campaign director, conversational crew,
and self-operating servers — while degrading gracefully to fully scripted behaviour when no
model is configured.

| Epic | Theme | Phase |
|---|---|---|
| M | Agentic AI substrate: provider seam, world-state API, MCP surface ([#589](https://github.com/fighters-legacy/fighters-legacy/issues/589)) | 4→5 |
| N | Dynamic campaign director — `fl-director`, between-session theater evolution, war-diary chronicle, GM conjure-mission ([#590](https://github.com/fighters-legacy/fighters-legacy/issues/590)) | 4→5 |
| O | Conversational crew & command — wingman NL, GCI/AWACS, radio chatter ([#591](https://github.com/fighters-legacy/fighters-legacy/issues/591)) | 4→6 |
| P | Agentic server operations — `fl-ops` ([#592](https://github.com/fighters-legacy/fighters-legacy/issues/592)) | 5→6 |

**Architecture principles (locked):** (1) LLMs never run in the 60 Hz tick — agents act at
human timescale against a ~1 Hz world-state snapshot + event stream; (2) agents act **only
through validated paths** — the admin command surface (MCP, with a server-side allowlist),
mission YAML through `validate-mission`, and the `AiControllerFactory`/`StateMachineController`
grammar — never direct state mutation; (3) every feature degrades gracefully to scripted
behaviour with no provider configured, and that fallback is the CI-tested path (**CI never
requires a model**); (4) ops autonomy is tiered (observe → recommend → act-with-allowlist);
(5) player chat and names are untrusted input to agents (prompt-injection hardening).

**Provider seam:** any OpenAI-compatible endpoint; the reference deployment is local
(Ollama / llama.cpp server); operators may point at vLLM or a hosted API — never required.
Mirrors the pluggable `IIdentityProvider` pattern. **New repos (Go):** `fl-director`,
`fl-ops` — the same polyglot service boundary as `fl-account`/`fl-review`/`fl-operator`;
each ships win/mac/linux binaries plus Linux container images.

**Dependency order:** **M leads** — every other epic consumes its world-state API and
MCP/admin surface. N follows the Phase 4 mission & campaign runtime epic
([#584](https://github.com/fighters-legacy/fighters-legacy/issues/584)) and deepens with
Epic H theater memory. O's text path follows Epic E chat
([#646](https://github.com/fighters-legacy/fighters-legacy/issues/646)) and Epic F's shared
track picture (#528); its voice path follows Epic J capture (#531) plus local STT. P consumes
Epic G metrics and Epic K fleet control, so it runs last. **Spikes first:** every epic opens
with a time-boxed spike proving local-model viability for its narrow question before feature
work is scheduled.

**Acceptance (initiative gate):** with **no provider configured**, all Phase 4–6 gameplay/ops
acceptance passes unchanged (scripted fallbacks are a tested path). With a **local ≥ 9B instruct
model** (measured — 9B is the floor at which these workloads work; the initiative's original
"7–8B" assumption did not survive the #599 sweep): the director produces a `validate-mission`-clean
mission from live campaign state in ≤ 60 s; a natural-language wingman command maps to a correct
grammar action on ≥ 90% of the intent test set; the ops agent triages an induced tick-overrun to a
correct bounded allowlisted action with zero unauthorized commands. Sim tick p99 is unchanged with
agents attached (out-of-tick guarantee, validated under Epic I load).

Two of those hold on the **CPU-only** reference instance and one does not — accuracy is a property
of the model, not the host, so the gap is purely latency (`docs/developer/decisions/ai-provider-evaluation.md`). The
**wingman's ≥ 90% intent gate is measured on a GPU-backed provider**: no model is both accurate
enough and inside the 2 s radio-comms budget on CPU, so the natural-language wingman is a **GPU
feature** and CPU-only servers ship the scripted command grammar (#610) — decided in #769,
recorded in `docs/developer/ai-architecture.md` §9. The **director's 60 s** figure is a between-mission
timescale, not a synchronous call: it is missed on CPU at exactly the model sizes worth using, so
`fl-director` generates mission *N+1* while *N* is flown.

Between the scripted menu and the GPU-only LLM intent tier sits a **deterministic voice-command
tier** — push-to-talk whisper.cpp STT fuzzy-matched onto the six-command `WingmanCommand.h`
grammar. It runs no LLM, is CPU-viable, is **independent of the #769 GPU decision**, and lets
*every* server offer "talk to your wingman" with the scripted fallback intact; the LLM intent
tier is the GPU upgrade layered on top. Both STT (whisper.cpp) and TTS (Piper, for GCI/ATC/
wingman replies and radio chatter) are CPU-real-time and outside the 9B/GPU model floor.

---

## Critical Path

**Landed (Phase 3 gate, `v0.3.0`):** spatial partitioning (#360) → interest management (#346);
the load harness (Epic I) + data-parallel sim tick (Epic A: #510–#514) + wire quantization
(Epic B: #515/#402/#516/#517/#566/#518) that proved the 128-client tick/bandwidth budget on the
8-core reference env ([#505](https://github.com/fighters-legacy/fighters-legacy/issues/505));
GameNetworkingSockets behind `INetwork` (Epic L: #506–#509, GNS 128-client gate #649); the
LuaSandbox → `IEntityController` seam (#359); the server-side AI framework (#352) and the
scripted wingman command grammar (#610).

**Landed since (`v0.3.1`–`v0.3.13`):** the spherical-Earth world (#468); the sensor →
avionics/HUD → padlock chain (Epic F #498, #587, #671); the mission and campaign runtime (#584);
the MP gameplay framework and internet server browser (Epic E #497, #646/#647/#648, #143);
multi-crew aircraft (#966); the replay core with its determinism gate (#588, #644); and the
agentic-AI substrate (Epic M #589 stages). Chains 1–4 of the previous revision are fully closed
and have been pruned per the phase-gate close checklist above.

**One chain remains open toward `v0.4.0`:**

1. **fl-base-pack gating content (#54 — 13 issues: terrain, missions, audio, AI scripts, crew
   voice, campaign seed, training set, sensor defs, red air) ∥ the consolidation program
   (#1036 Stages 5–9: protocol hardening #1063, interface consolidation #1064; interim releases
   `v0.3.14+`) → pre-gate audits (documentation #1047 ✓, **gameplay #1065 last**) → Phase 4 gate
   (#729) → `v0.4.0`.** The two run in parallel — content lives in fl-base-pack, consolidation in
   the engine — and #54's sub-issue rollup is the cross-repo readiness gauge, now containing
   exactly the gating set.

**Later phases:** IGui HAL (#156) → in-game mission editor (#49) + subtitles (#165) + crash
overlay (#236) [P6]; persistence (Epic H) → identity (Epic C) → anti-cheat (Epic D) + operator
(Epic K) [P5]; platform packaging [P7] → OpenGL + headless renderer [P8]. Deployment artifacts
(#160 container image, #228 release packaging, #1096 systemd unit) open Phase 5 alongside Epic G,
ahead of the Epic K operator work that consumes them. Ops/agentic-cluster additions land in
Phase 5 after Epics G/K.

---

## Deferred Levers (armed triggers)

Some scaling and optimisation work is **designed and trigger-gated but deliberately not built** —
each is dominated by simpler work today and only pays off under a measurable condition. They live
here, in a single registry, rather than as perpetually-open issues that would rot on the board.
**Check this section whenever an Epic I scale gate fails.**

| Lever | Spike | Trigger | Design |
|---|---|---|---|
| Reduced-rate ("LOD") physics for distant AI | [#575](https://github.com/fighters-legacy/fighters-legacy/issues/575) | Integrate-bound: `load_factor` at floor + rising `dropped_ticks` + `integrate_ms.mean > 0.5 × tick_ms.mean` on the reference env under product workload | [physics-lod-design.md](developer/decisions/physics-lod-design.md) |
| Spatial sharding + pre-sharding ladder (shared snapshot encode, governor interest-radius lever #726) | [#572](https://github.com/fighters-legacy/fighters-legacy/issues/572) | Sustained `load_factor` at floor + rising `dropped_ticks` + serialize-dominant tick at max workers on the 8-core reference env (multi-machine is a Phase 5+ product decision) | [spatial-sharding-design.md](developer/decisions/spatial-sharding-design.md) + the 2026-07-10 [decision record](developer/architecture.md#decision-records) |
| enet6 retirement (drop the LAN/low-count backend) | [#652](https://github.com/fighters-legacy/fighters-legacy/issues/652) | GNS parity across LAN/single-player/low-count + Windows/macOS GNS CI legs stable (Phase 5) | [transport-selection.md](developer/decisions/transport-selection.md) |

Design-complete deferred levers are registered here, not as open issues; when a trigger fires,
the lever's spike is re-opened into an implementation epic.

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
- Wire protocol documented (`docs/developer/network-protocol.md`).
- enet6 backend active; fl-server binds on `::` dual-stack.
- Spherical-Earth world model functional; `CentralGravityField` and `TerrainStreamer` curvature correction active.
- CI green on all three platforms (debug, debug-msvc, macOS).

### Phase 3 — Engine Systems ✓

Phase 3 acceptance is a **complete engine layer** — all features testable with zero content packs.
**Verified at the `v0.3.0` gate (2026-07-10).** *(moved)* items were re-scoped forward on
2026-07-01 and are re-validated in their new phase.

> **2026-07-01 Phase 3 close-out re-scope** (see the
> [decision record](developer/architecture.md#decision-records)): the spherical-Earth epic (#468) and its
> sub-issues move to Phase 4 (it gates content, not engine seams); NVG (#210) and the FlightHud
> redesign (#438) move to the Phase 4 avionics epic (#587); renderer-advancement items
> (#443–#445, #448–#454) move to Phase 8 under epic #597 (milestone renamed "Rendering &
> Alternative Backends"); biome-texture work (#446/#447) joins #468. Bullets below marked
> *(moved)* are re-validated in their new phase. Phase 3 retains the scaling spine (Epics
> A/B/I/L), fuzzers (#94), layering CI (#559), and #439.

- Spatial partition queries functional; broadphase neighbor search passes unit tests.
- Snapshot interest management active; bandwidth under threshold with 20 simulated clients.
- LuaSandbox wired to `IEntityController`; a scripted sandbox entity responds correctly in tests.
- Server-side AI flight controller framework: at least one AI entity maintains altitude in sandbox.
- `FlightState::pos_world` is `double[3]`; all integrator math consistent with dvec3 world positions.
- Pilot profiles persist across sessions. *(Stats-at-debrief moved to Phase 4 — #634 under the
  mission & campaign runtime epic #584.)*
- Advanced quality settings: shadow resolution, particle density, and AA mode selectable and saved to user.toml.
- Per-vertex spherical terrain mesh correction: no visible seams or skirts at altitude.
- libFuzzer harnesses in CI; zero crashes on seed corpus.
- Connection heartbeat/keepalive: ENet peer timeout behaves correctly under packet loss.
- Client-side prediction active for the player entity; inputs feel responsive on 100 ms RTT connections without visible snapping artifacts.
- `bindings.toml` loaded; per-axis HOTAS/gamepad mapping applied at startup.
- WeatherPreset::Snow and WeatherPreset::Blizzard functional (weather state machine + visual presets).
- NVG cockpit overlay toggles on/off in cockpit mode. *(Moved to Phase 4 — #210 under the
  avionics epic #587.)*
- Scaling seams landed: transport replacement (Epic L) selected (GameNetworkingSockets, #506)
  behind `INetwork` and passing
  a transport scale-spike; load-test bot-swarm harness + the perf/soak scale gate (Epic I, #520 ✓)
  run in CI; server tick-budget
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
- Replay core (#588): a mission records to `.flrep` (quantized snapshot stream) and plays back
  from cockpit and free-camera views, with the determinism gate (#644) landing alongside the
  campaign engine. *(ACMI/Tacview export and killcam ride Phase 5.)*
- Multiplayer: two clients on fl-server complete a cooperative strike mission.
- MP gameplay framework (Epic E): a data-driven game mode (e.g. team deathmatch) runs a full
  match lifecycle (warmup → active → end → rotation) with team assignment, scoring, spectator,
  and drop-in/reconnect.
- Sensor framework (Epic F): radar search/track + IFF + a shared team track picture function
  against fl-base-pack content.
- Scale proven: Epics A/B/I demonstrate the 128-client tick + bandwidth gate (see the
  cross-cutting initiative), and the `reference` profile passes **with sensor content loaded** —
  bots fly `builtin:sensor-fighter` against live hostiles, so contact tables populate and datalink
  team fusion actually runs (#1089). The shipped 128-client claim describes a populated battlespace,
  not a hollow world. Builtin sensors satisfy this, so the gate never blocks on pack content. A
  prerequisite for Phase 4 multiplayer acceptance.
  - ⚠ **Voice relay is NOT part of this gate**, though it was claimed here until the v0.4.0 audit.
    `bot_swarm` has no voice generator, so every reference run has reported
    `voice_relay_sends: 0` and the clause was never once exercised. The server-side counter exists
    (#1090); the load generator does not. Building it is M5.0 work, tracked in #1379 — and the
    criterion now describes what is measured rather than what was intended.
  - ⚠ **The `≤ 16.6 ms p99` tick assertion is advisory on a virtualized reference runner** and is
    hard-gated only on bare metal (#1379). The VM's own noise floor is ~24 ms — larger than the
    threshold — so it measures the hypervisor, not the engine; the same load on bare metal measures
    4.72 ms. Bandwidth, admission and the KB/s baseline are machine-independent and stay hard-gated
    everywhere. A dedicated non-virtualized reference box is the open follow-on under #496.
- Connect-path and message hygiene: every client→server message type is rate-limited or
  one-shot-gated, and no unauthenticated request produces an amplified reply (2026-07-30 review;
  epic #1063).
- Version honesty: the build version travels in the hello/beacon/server-info surfaces and the
  server browser flags a build mismatch, so two builds that both advertise `kProtocolVersion = 1`
  cannot silently diverge (#1074).
- In-match text chat (team/all), kill feed, scoreboard, and respawn/slot management function
  in a 128-client match (Epic E extension: #646/#647/#648).
- Full kill chain: fire → guidance → warhead → damage → kill credit → debrief stats, with
  lag-compensated hit detection (weapons epic #583).
- Agentic AI substrate live (world-state API + MCP surface, Epic M #589); the M/N/O spikes
  concluded; no-provider degradation validated (all AI features fall back to scripted
  behaviour — the CI-tested path).
- Server discovery posture: LAN discovery, direct connect, and the client's federated internet
  server browser (#143; `[client] lobby_urls`, empty by default) are the deliberate matchmaking
  model — no skill-based matchmaking service (self-host posture). **v0.4.0 ships the client
  capability**; the fl-lobby reference listing service is Phase 5 (#999).
- Multi-crew aircraft (Epic #966): a crewed airframe spawns with every authored seat bot-filled
  (or empty per authoring); a human joins any non-human seat at connect or in-session and drives
  only that seat's capabilities; on vacate/disconnect the seat reverts to its authored default with
  no visible discontinuity (a bot pilot resumes mid-maneuver); humans never displace humans and the
  airframe is never destroyed out from under a remaining human occupant; a defensive turret aims and
  fires independently of the airframe nose within its traverse limits and a gunner's kill credits the
  gunner; single-seat aircraft stay byte-identical on the wire. The turret servo delivers the
  ground-SAM **launcher-elevation gap** documented as owed to #585 (a static emplacement mounts its
  launcher as a turret). Provable zero-pack via `builtin:bomber` in the armed sandbox.
- Helicopter and multirotor force models functional with appropriate fl-base-pack aircraft types.
- Per-engine failure simulation: L/R bits produce asymmetric thrust effects in cockpit.
- Afterburner envelope limits enforced per aircraft TOML definition.
- Lua scripting API documented (`docs/modding/ai.md`).
- ~~**Demo videos (#909):** the v0.4.0 cinematic demo set (`missions/demos/`) records headless and is
  reviewed + attached to the `v0.4.0` Release.~~ **Dropped from the v0.4.0 gate (owner, 2026-08-31).**
  The recorder's engine defects are fixed (#1381 — an observer's interest centre never followed the
  camera, so anchored missions recorded an *empty world*; and a flat-Earth camera up vector canted
  their horizon), and 9 of 10 demos now record clean. But reviewing one frame from each showed the
  set is not release-quality: subjects are specks on the horizon, the carrier is not visible in
  `demo-carrier-swarm`, the SAM site is not visible in `demo-sam-strike`, and `demo-formation-tour`
  shows a single aircraft. **Nine of those pass duration + blackdetect + freezedetect**, because
  those checks measure liveness, not composition — no threshold closes that gap, and only looking
  does. Reframing the shots is authoring work (#1378), not debugging, and is M5.0.
  A later phase gate re-adopts this line once the set is worth publishing.
- CI green on all three platforms.

### Phase 5 — Multiplayer at Scale & Live Services

Phase 5 acceptance is the **live-services tier** that makes 128-player public/community servers
operable, identifiable, and cheat-resistant. Engine-layer scaling seams (transport, job system,
wire quantization, load harness) are validated earlier as Phase 3–4 gates.

Phase 5 opens by authoring its **execution plan** — the #1036 pattern, filed as the milestone's
first Task — so its ~70 open issues are staged there rather than here.

- Transport replacement (Epic L) holds the Epic I scale gate: 128 clients @ 60 Hz, sim tick
  ≤ 16.6 ms p99 on the reference instance, soak-stable for 2 h.
- Quantized snapshot stream + priority/budget scheduling keep sustained downstream
  ≤ ~150 KB/s/client at 128 players.
- Server-side identity: a client authenticates via a pluggable `IIdentityProvider`
  (offline-verifiable signed token); guest play still allowed when the server permits it.
- Persistence (`IPersistence`): accounts, stats, and bans survive restart; file banlists import.
- Anti-cheat: live input validation rejects impossible states in-tick; the offline `fl-review`
  pipeline flags a seeded cheat corpus; **content-pack hashes are verified at join** (#1098 — the
  reserved `contentHash` wire field becomes enforced).
- Observability: `fl-server` exports Prometheus metrics; bundled Grafana dashboards render; the
  admin web interface performs kick/ban/config-reload against a running server.
- Admin surfaces: the HTTP admin and MCP listener serves TLS natively, or refuses a non-loopback
  bind without it (#1097); RCON is dual-stack (#316).
- Deployment artifacts: the official `fl-server` container image publishes to GHCR (#160),
  `fl-server` ships in the release archives (#228), and a systemd unit is provided (#1096) —
  moved forward from Phase 7 by the 2026-07-30 review so the hosting artifacts precede the
  operator that consumes them.
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
- Crash reporting operational on all three platforms; reports reach the configured endpoint.
- All three platforms in CI green with release artifacts attached.

### Phase 8 — Rendering & Alternative Backends

- OpenGL 4.1 Core backend: all seven render passes functional on Mesa + ANGLE + Intel iGPU.
- Software/headless renderer: CI visual regression tests run without a physical GPU.
- Golden-image regression harness (#451) runs in CI before any renderer-quality change lands.
- Renderer advancement (epic #597): temporal AA/upscaling, sky LUTs, IBL, and AO-quality items
  land behind the existing quality-settings enums.

### Phase 9 — Modding Platform

- GPG signature verification for community and maintainer content packs.
- SHA-256 manifest hash pinning enforced on update; tampered packs rejected.
- Subprocess isolation active for compiled content plugins on all three platforms.
- In-game mod browser: lists, installs, enables, and disables packs without restart.
- First-run fl-base-pack download from community index completes successfully.
- `validate-mod` passes on fl-base-pack.
- Modding documentation complete; content pack authoring guide published.
