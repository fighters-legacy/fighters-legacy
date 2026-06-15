# Roadmap

Development is tracked through [GitHub milestones](https://github.com/jomkz/fighters-legacy/milestones).
Each phase has a milestone with individual issues for every workstream task.

## Schedule

Phases are sequentially gated. Week numbers from the original plan are removed — they
drifted from reality during Phase 2. Ordering constraints are listed instead.

| Phase | Name | Gate |
|---|---|---|
| 1 — Engine Foundation ✓ | HAL, content system, CI/CD | — |
| 2 — Modern-Particles Engine ✓ | Game loop, flight model, networking, renderer, spherical Earth | Phase 1 complete |
| 3 — Engine Systems | Spatial partitioning, AI framework, interest management, bindings, quality settings | Phase 2 complete |
| 4 — Content & Gameplay | fl-base-pack content, radar/weapons, AI system, missions, multiplayer, advanced vehicle models | Phase 3 complete + fl-base-pack substantially ready |
| 5 — UI Layer & Tooling | IGui HAL + Dear ImGui, in-game mission editor, welcome screen | Phase 4 complete |
| 6 — Platform Release | macOS/Linux/Windows packages, Flathub, fl-server container, crash reporting | Phase 5 complete |
| 7 — OpenGL & Alternative Renderers | OpenGL 4.1 Core, headless/software renderer for CI, voice chat | Phase 6 complete |
| 8 — Modding Platform | GPG verification, subprocess isolation, in-game mod browser, community content distribution | Phase 7 complete |

---

## Critical Path

1. **Spatial partitioning (#360) → interest management (#346) → multiplayer at scale (Phase 4)**
   Broadphase index enables range queries needed for interest management; both must be in
   before Phase 4 multiplayer acceptance testing with real clients.

2. **LuaSandbox wired (#359) → fl-base-pack AI scripts → AI System (#33)**
   fl-base-pack Lua behaviour scripts have nothing to run against until `IEntityController`
   integration is complete in Phase 3.

3. **Server-side AI framework (#352) → AI System (#33) → radar/weapons AI (#42)**
   Flight controller framework is the substrate; the full AI system and radar/weapons AI
   build on top of it in Phase 4.

4. **fl-base-pack #1–#6 (aircraft, terrain, missions, audio, AI) → Phase 4 acceptance testing**
   fl-base-pack content work can start in parallel with Phase 3 (except AI scripts, which
   wait on #359). Phase 4 acceptance is gated on fl-base-pack being substantially complete.

5. **IGui HAL (#156) → in-game mission editor (#49), subtitle rendering (#165), crash overlay (#236)**
   All Phase 5 features depend on the IGui interface being stable.

6. **Platform packaging (Phase 6) → OpenGL renderer (Phase 7)**
   OpenGL is an optional compatibility layer; it should not block the primary release.
   The headless/software renderer (Phase 7) enables GPU-free CI visual regression tests.

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
- `FlightState::pos` is `double[3]`; all integrator math consistent with dvec3 world positions.
- Pilot profiles persist across sessions; stats updated at mission debrief.
- Advanced quality settings: shadow resolution, particle density, and AA mode selectable and saved to user.toml.
- Per-vertex spherical terrain mesh correction: no visible seams or skirts at altitude.
- libFuzzer harnesses in CI; zero crashes on seed corpus.
- Connection heartbeat/keepalive: ENet peer timeout behaves correctly under packet loss.
- `bindings.toml` loaded; per-axis HOTAS/gamepad mapping applied at startup.
- WeatherPreset::Snow and WeatherPreset::Blizzard functional (weather state machine + visual presets).
- NVG cockpit overlay toggles on/off in cockpit mode.
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
- Helicopter and multirotor force models functional with appropriate fl-base-pack aircraft types.
- Per-engine failure simulation: L/R bits produce asymmetric thrust effects in cockpit.
- Afterburner envelope limits enforced per aircraft TOML definition.
- Lua scripting API documented (`docs/modding/ai.md`).
- CI green on all three platforms.

### Phase 5 — UI Layer & Tooling

- IGui HAL implemented with Dear ImGui backend on all three platforms.
- In-game mission editor: create, edit, and save a YAML mission on all three platforms.
- Round-trip: create a TOML aircraft + glTF mesh; load it in the engine without errors.
- Subtitle text rendering via IGui overlay functional.
- First-run welcome screen shown on initial launch.
- Crash report and mod-load failure overlay display correctly via IGui.

### Phase 6 — Platform Release

- macOS .app bundle signed and notarized; passes Gatekeeper on a clean machine.
- Linux Flatpak published to Flathub; AppImage available for direct download.
- Windows Inno Setup installer; statically-linked VCRT; no external DLL requirement.
- fl-server official container image published to GHCR.
- Crash reporting operational on all three platforms; reports reach the configured endpoint.
- All three platforms in CI green with release artifacts attached.

### Phase 7 — OpenGL & Alternative Renderers

- OpenGL 4.1 Core backend: all seven render passes functional on Mesa + ANGLE + Intel iGPU.
- Software/headless renderer: CI visual regression tests run without a physical GPU.
- In-game voice chat functional in multiplayer sessions on all three platforms.

### Phase 8 — Modding Platform

- GPG signature verification for community and maintainer content packs.
- SHA-256 manifest hash pinning enforced on update; tampered packs rejected.
- Subprocess isolation active for compiled content plugins on all three platforms.
- In-game mod browser: lists, installs, enables, and disables packs without restart.
- First-run fl-base-pack download from community index completes successfully.
- `validate-mod` passes on fl-base-pack.
- Modding documentation complete; content pack authoring guide published.
