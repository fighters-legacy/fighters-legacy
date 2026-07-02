# Design Pillars & Lifted Constraints

## Gameplay Design Pillars

These are the non-negotiable design values that resolve every ambiguous feature decision.

- **Arcade-to-sim balance**: Closer to Ace Combat / Project Wingman than DCS World. Physics
  are simplified but feel consequential. Stalls, G-effects, and energy management matter;
  startup checklists, instrument-only navigation, and systems management do not.
  See [prior-art.md](prior-art.md) for the competitive landscape that motivates this choice.
- **Depth through variety, not complexity**: Replayability comes from varied missions,
  wingman decisions, loadout tradeoffs, and campaign outcomes — not from the number of
  cockpit buttons or procedures.
- **Direct, action-mapped controls**: Many key bindings, each with a clear and immediate effect.
  Players can be effective with a keyboard; HOTAS rewards precision without being required.
- **Approachable by default**: New players should be in the air and shooting within minutes
  of first launch. Every simulation-leaning feature has a difficulty toggle that makes it
  optional.
- **Single-player first, multiplayer co-equal**: A rich solo experience (campaign, instant
  action, training) is a first-class foundation — *and* large-scale multiplayer is a co-equal
  product pillar, not a secondary extension. PvP (squadron/team battles), co-op PvE (shared
  strike/campaign missions), and persistent-world play are all in scope. The architectural
  target is **128+ simultaneous players**, which makes scalability, server-side identity, and
  anti-cheat first-class concerns from the engine layer up — not late-phase afterthoughts.
- **Tools, not rules**: The engine exposes capabilities; players decide what to do with
  them. No content is locked behind progression in sandbox mode. The campaign layer is
  an optional narrative experience layered on top of a fundamentally open simulation —
  not the foundation it depends on. Players can ignore the campaign entirely and still
  have a complete game.
- **Platform for community**: Mission editor, open asset formats, mod system, and a Lua
  scripting API make Fighters Legacy a platform people build content on, not just a game
  they consume. The tools developers use to build the game are the same tools players and
  modders use. There is no privileged content pipeline.
- **Dynamic world, optional intelligence**: Agentic AI — a campaign director, conversational
  crew, self-operating servers — runs local-first through a pluggable provider and always
  degrades to scripted behaviour. Intelligence is an amplifier, never a dependency: the game
  is complete with no model configured.

---

## Engine Capabilities

Design choices that explicitly avoid constraints common in older games of this genre.

| Area | Engine Design |
|---|---|
| Theater size | Arbitrary; streaming terrain chunks |
| Object pool | Dynamic allocation; soft limits tunable per server |
| Multiplayer topology | Dedicated `fl-server`; optional `fl-lobby` matchmaking |
| Multiplayer players | 128+ architectural target (server-authoritative); 32 is the near-term acceptance floor |
| Campaign structure | Arbitrary YAML graph; branching, nested objectives, any count |
| AI scripting | Lua 5.5; full scripting API; multiple concurrent behaviors |
| Score / rank tiers | Data-driven; TOML-defined rank tables |
| Aircraft count | Unlimited; all content pack entries |
| Weapon hardpoints | Configurable per aircraft TOML |
| Audio sample rates | OGG at any rate; arbitrary sample rate |
| Render resolution | Any resolution; windowed or fullscreen |

---

## Multiplayer at Scale

The 128+ player target turns several engine choices into non-negotiables. These are
first-class concerns, designed in from the engine layer rather than retrofitted:

- **Server-authoritative and cheat-resistant.** The client never owns physics or state.
  Live input validation rejects impossible states in-tick; heavier statistical detection runs
  offline against logs/replays. No kernel anti-cheat — server authority plus offline review
  is the right posture for a GPL, self-hostable game.
- **Bounded per-client bandwidth.** Quantized/bit-packed wire state plus per-client
  priority/budget snapshot scheduling and 3D interest management keep bandwidth flat as player
  count grows, rather than scaling with the square of the population.
- **Parallel simulation.** The authoritative tick parallelizes per-entity integration and AI
  across cores (data-parallel job system) so 128 players + AI + projectiles hold 60 Hz.
- **Server-side identity.** Players authenticate against a pluggable identity provider
  (offline-verifiable signed tokens); persistent stats, ranking, and bans key on a verified
  account, not a spoofable client GUID. Self-hostable; no first-party hosted infra required.
- **Live-ops observability.** Servers export metrics and structured logs; clustered fleets are
  managed by a Kubernetes/OpenShift operator with health, autoscaling, and graceful session
  draining.

Self-hosting is the default: communities run their own servers and identity. The project
ships the software; it does not operate central infrastructure.

---

## Dynamic World & Agentic AI

A second cross-cutting initiative (Epics M–P, decision record 2026-07-01) layers modern
agentic-AI patterns onto the deterministic core — in ways the genre has not shipped — under
five non-negotiable principles:

- **LLMs never run in the 60 Hz tick.** Agents act at human timescale against a ~1 Hz
  world-state snapshot and event stream, assembled off the sim thread.
- **Validated paths only.** Agents act through the same surfaces humans use — the allowlisted
  admin/MCP command surface, mission YAML gated by `validate-mission`, and the AI behaviour
  grammar. No direct state mutation, ever.
- **Graceful degradation.** Every AI feature has a scripted fallback that is the CI-tested
  path; CI never requires a model.
- **Tiered ops autonomy.** observe → recommend → act-with-allowlist for server operations.
- **Untrusted player text.** Chat and names entering a prompt are data, not instructions.

What this buys: a campaign director that generates and narrates missions from live theater
state; wingmen and GCI you can talk to; servers that triage their own incidents. The provider
is pluggable and local-first (Ollama / llama.cpp reference; any OpenAI-compatible endpoint) —
consistent with the self-host pillar. Full design in
[docs/ai-architecture.md](ai-architecture.md).

---

## Settings Philosophy

These principles govern what appears in the settings UI and how settings are stored.

**Approachable over exhaustive.** Only settings a non-technical player can act on meaningfully without reading a manual belong in the primary settings screen. Settings that require understanding of rendering techniques or hardware internals are deferred to an "Advanced" section in a future phase.

**Quality preset, with individual overrides.** Phase 1 controlled shadow quality, particle density, and anti-aliasing solely through a single quality preset, deferring per-feature controls to keep the screen approachable. As the Vulkan render graph matured these returned as individual settings: shadow quality, particle density, and AA mode (Phase 2, #235), then ambient-occlusion and sky quality (Phase 3, #437). The quality preset remains the high-level convenience knob; the per-feature rows live alongside it for players who want them.

**Draw distance is an exception.** It is gameplay-critical in a flight sim — seeing a radar contact or a bandit at distance affects situational awareness directly. It is exposed as a primary setting regardless of the quality preset.

**Anti-aliasing is a named choice: Off / FXAA / TAA.** Phase 1 exposed AA as a simple on/off toggle. The render graph now offers explicit modes; MSAA was dropped in favour of TAA (it resolves shading/specular aliasing MSAA cannot and is the on-ramp to temporal upscaling), so the modes are Off, FXAA, and TAA, with TAA the default.

**Audio sliders store integers.** Volumes are stored as integers 0–100 in `config/user.toml`, matching the "0–100%" range in the UI spec, and converted to float 0.0–1.0 at runtime for `IAudio::setGain()`. This avoids float serialization precision issues and keeps the file human-readable and hand-editable.
