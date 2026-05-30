# Roadmap

Development is tracked through [GitHub milestones](https://github.com/jomkz/fighters-legacy/milestones).
Each phase has a milestone with individual issues for every workstream task.

## Schedule

| Phase | Weeks | Key Dependency |
|---|---|---|
| 1 — Engine Foundation | 1–8 | None (start immediately) |
| 2 — Modern-Particles Engine | 10–36 | HAL stable; fl-base-pack bootstrap started |
| 3 — OpenGL Compatibility Renderer | 37–50 | Phase 2 engine stable |
| 4 — In-Game Mission Editor | 26–38 | Phase 2 Vulkan HAL stable (can begin in parallel from week 26; Phase 2 acceptance gates full ramp-up) |
| 5 — Linux/macOS Release | ongoing → ~44 | All CI green; MoltenVK verified |
| 6 — Native Formats & Modding | 36–50 | modding docs complete |

Total estimated duration: **~48–56 weeks** of focused work.

### Phase 1 Workstreams

| Workstream | Weeks | Dependency |
|---|---|---|
| 1A — Engine Core Setup | 1–8 | None (start immediately) |

---

## Critical Path

1. **`IContentPack` interface (A.3) → fl-base-pack scaffold → asset pipeline tools → Phase 2 acceptance testing**
   Interface must be stable before fl-base-pack assets and engine subsystems depend on it.
   Lock the interface in week 2; do not break it after that.

2. **Weapon TOML format → loadout screen (2.5) → dynamic campaign unit pools (2.5)**
   Weapon IDs must be stable before the loadout screen, AI weapons selection, and
   dynamic campaign mission templates can reference them.

3. **Ground/naval unit TOML format → dynamic campaign (2.5) → sandbox unit spawning (2.14)**
   Unit IDs and AI script contracts must be defined before the dynamic campaign generator
   can instantiate unit pools.

4. **Radar & EW (2.13) → AI weapons selection (2.4) → multiplayer balance (2.7)**
   AI must know missile guidance types to select correct weapons and respond to jamming.
   EW parameters affect game balance in multiplayer; lock before server-side enforcement.

5. **HUD (#2.16) + Main menu (#2.17) → Phase 2 early-access milestone**
   Players must be able to launch, play a mission, and exit without a command line.
   These are required for the itch.io early-access release and gate Phase 2 acceptance.

---

## Verification / Acceptance Criteria

### Phase 1 — Engine Foundation

- All three CI jobs build clean (Windows/Linux/macOS).
- Vulkan validation layers: zero errors on triangle hello-world.
- MoltenVK smoke test passes on Apple Silicon.
- Engine boots cleanly with zero content packs installed; sandbox inspector reachable.
- First-run logic layer present (#22, merged); rendered welcome screen deferred to Phase 4 (#98); content download deferred to Phase 6 (#108).

### Phase 2 — Modern-Particles Engine

- A fl-base-pack mission loads and runs to completion via `ModLoader`.
- Flight model stall speed + fuel burn match design spec for each fl-base-pack aircraft type.
- Radar lock, missile fire, and countermeasure sequence works per fl-base-pack weapon definitions.
- Progressive damage: light / heavy / critical thresholds produce correct visual + penalty.
- In-flight refueling: all three tiers (auto / simplified / manual) functional.
- Wingman commands: all six commands acknowledged and executed by AI.
- Dynamic campaign: frontline advances after objective completion; story mission injects at trigger.
- Sandbox mode: configurable start, no win condition, session saves and resumes.
- Game Master: entity spawn/despawn and weather change take effect in running session.
- Replay: mission records and plays back from cockpit and free-camera views.
- GPU particles render for explosion / smoke / fire.
- Multiplayer: two clients on fl-server complete a cooperative strike mission.
- CI green on all three platforms.

### Phase 4 — In-Game Mission Editor

- In-game mission editor: create, edit, and save a YAML mission on all three platforms.
- Round-trip: create a TOML aircraft + glTF mesh; load it in the engine.

### Phase 6 — Native Formats & Modding

- A mission using only native YAML + glTF + TOML + OGG content runs without
  any third-party content plugin installed.
- `validate-mod` passes on fl-base-pack.
