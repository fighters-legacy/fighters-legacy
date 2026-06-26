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
- **Single-player first, multiplayer equal**: A rich solo experience (campaign, instant
  action, training) is the foundation. Multiplayer extends it; it is not the primary draw.
- **Tools, not rules**: The engine exposes capabilities; players decide what to do with
  them. No content is locked behind progression in sandbox mode. The campaign layer is
  an optional narrative experience layered on top of a fundamentally open simulation —
  not the foundation it depends on. Players can ignore the campaign entirely and still
  have a complete game.
- **Platform for community**: Mission editor, open asset formats, mod system, and a Lua
  scripting API make Fighters Legacy a platform people build content on, not just a game
  they consume. The tools developers use to build the game are the same tools players and
  modders use. There is no privileged content pipeline.

---

## Engine Capabilities

Design choices that explicitly avoid constraints common in older games of this genre.

| Area | Engine Design |
|---|---|
| Theater size | Arbitrary; streaming terrain chunks |
| Object pool | Dynamic allocation; soft limits tunable per server |
| Multiplayer topology | Dedicated `fl-server`; optional `fl-lobby` matchmaking |
| Multiplayer players | 32+ (server-authoritative) |
| Campaign structure | Arbitrary YAML graph; branching, nested objectives, any count |
| AI scripting | Lua 5.5; full scripting API; multiple concurrent behaviors |
| Score / rank tiers | Data-driven; TOML-defined rank tables |
| Aircraft count | Unlimited; all content pack entries |
| Weapon hardpoints | Configurable per aircraft TOML |
| Audio sample rates | OGG at any rate; arbitrary sample rate |
| Render resolution | Any resolution; windowed or fullscreen |

---

## Settings Philosophy

These principles govern what appears in the settings UI and how settings are stored.

**Approachable over exhaustive.** Only settings a non-technical player can act on meaningfully without reading a manual belong in the primary settings screen. Settings that require understanding of rendering techniques or hardware internals are deferred to an "Advanced" section in a future phase.

**Quality preset, with individual overrides.** Phase 1 controlled shadow quality, particle density, and anti-aliasing solely through a single quality preset, deferring per-feature controls to keep the screen approachable. As the Vulkan render graph matured these returned as individual settings: shadow quality, particle density, and AA mode (Phase 2, #235), then ambient-occlusion and sky quality (Phase 3, #437). The quality preset remains the high-level convenience knob; the per-feature rows live alongside it for players who want them.

**Draw distance is an exception.** It is gameplay-critical in a flight sim — seeing a radar contact or a bandit at distance affects situational awareness directly. It is exposed as a primary setting regardless of the quality preset.

**Anti-aliasing is a named choice: Off / FXAA / TAA.** Phase 1 exposed AA as a simple on/off toggle. The render graph now offers explicit modes; MSAA was dropped in favour of TAA (it resolves shading/specular aliasing MSAA cannot and is the on-ramp to temporal upscaling), so the modes are Off, FXAA, and TAA, with TAA the default.

**Audio sliders store integers.** Volumes are stored as integers 0–100 in `config/user.toml`, matching the "0–100%" range in the UI spec, and converted to float 0.0–1.0 at runtime for `IAudio::setGain()`. This avoids float serialization precision issues and keeps the file human-readable and hand-editable.
