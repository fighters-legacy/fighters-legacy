# Fighters Legacy — Claude Code Instructions

**CLAUDE.md links, `docs/` states.** This file carries what you need to *operate* in this repo —
build and test invocation, conventions, layering rules, the traps that cost real time. Everything
that describes how a subsystem *works* lives in `docs/`, which is a published site with a drift
check ([Key documents](#key-documents)). If you find yourself explaining a subsystem here, it
belongs there instead.

## Project Overview

GPL v3 general-purpose combat flight sim engine, inspired by Jane's Fighters Anthology (1998).
Cross-platform: Windows 10/11, Linux, macOS. **Phase 4 is active** — the gameplay epics, the
spherical-Earth epic (#468) and the Dynamic World & Agentic AI initiative all run here, ahead of a
phase gate (#729) whose execution plan is issue **#1036**. Renderer-advancement items are Phase 8
("Rendering & Alternative Backends", epic #597); large-scale multiplayer live services are Phase 5.
See [`docs/roadmap.md`](docs/roadmap.md) for the phase schedule and per-phase acceptance criteria,
and [`docs/developer/project-management.md`](docs/developer/project-management.md) for issue types,
epics and the board conventions.

Legacy umbrella issues #33/#34/#42/#50 and the pre-epic stubs #130/#144/#145/#249 are closed as
superseded — don't reference them for new work.

**The engine is fully content-agnostic.** It knows nothing about FA or any specific game. FA support
lives in `fighters-legacy/fa-bridge`; no FA-specific code belongs in this repo. `ft-gui` is an old
name for `fighters-codex`, a separate project with no role here.

**Companion repositories** (under the `fighters-legacy` org; engine/game/server stay C++):
`fl-base-pack` (starter content, exists), and the planned **Go** services `fl-account` (identity),
`fl-review` (offline anti-cheat), `fl-operator` (k8s/OpenShift + Helm), `fl-director` (dynamic
campaign) and `fl-ops` (agentic server operations).

## Architecture

The layered model, the module-boundary policy and every decision record live in
[`docs/developer/architecture.md`](docs/developer/architecture.md). The map below is only for
finding your way to a directory.

```
engine/          — the simulation. Content-agnostic; links no client backend.
  content/       — content packs, AssetManager, ContentIndex, ContentBootstrap
  entity/        — entity pool, type registry, damage, collision, ejection, crew/seats
  weapon/        — weapon defs, the fire path, projectiles, seekers, turrets
  sensor/        — sensor defs + the detection machine (one vocabulary, three consumers)
  flight/        — flight model, force models, integrator, atmosphere, geodesy, gravity
  ai/            — server-side controllers, wingman grammar, state machines, guidance
  script/        — Lua 5.5 sandbox + LuaController + the world API seam
  net/           — server sim, wire protocol, snapshot codec, scheduler, congestion, governor
  world/         — factions, formations, airports, airspace zones, alert system
  mission/       — mission runtime; campaign/ — the deterministic campaign runtime
  render/        — sim→render bridge, SceneRenderer, terrain streaming, HUD, particles
  audio/         — SFX, music, warning tones (builtin procedural PCM; no content required)
  input/         — the binding table: the authority for every gameplay control
  replay/        — the .flrep codec (reader ships in the game AND the determinism gate)
  perf/ console/ config/ crypto/ i18n/ spatial/ job/ weather/ manual/ sandbox/
platform/        — the HAL: Vulkan, SDL3, OpenAL, network backends, HTTP, filesystem, subprocess
server/fl-server/— the authoritative headless server binary
game/            — the fighters-legacy client binary
tools/           — validators, generators, load/measurement harnesses (docs/developer/tools.md)
tests/           — Catch2 (C++) + pytest (Python tools)
fuzz/            — libFuzzer harnesses (clang-only `fuzz` preset, OFF by default)
```

**GLM** is the shared vector/matrix/quaternion library, linked as an INTERFACE dependency on
`platform-hal` — anything linking `platform-hal` gets it automatically.

## Build

```bash
# Linux / macOS
cmake --preset debug && cmake --build --preset debug

# Windows (PowerShell)
cmake --preset debug-msvc && cmake --build --preset debug-msvc

# Run tests
ctest --preset debug --output-on-failure
```

Prerequisites, every preset, sanitizers, coverage, fuzzing and the CI workflow structure are in
[`docs/developer/development.md`](docs/developer/development.md).

## Conventions

- Conventional Commits — scopes: engine / renderer / audio / network / content / i18n / flight /
  difficulty / entity / ai / mission / game / tools / build / ci / docs
- DCO sign-off required: `git commit -s`
<!-- REUSE-IgnoreStart -->
- SPDX header required on all new .cpp/.h files: `// SPDX-License-Identifier: GPL-3.0-or-later`
- Shader files (`.vert`, `.frag`, `.comp`, `.glsl`) are covered by REUSE.toml — no inline SPDX
  needed, but **add any new shader extension to REUSE.toml** or the REUSE CI step will fail.
- Python scripts (`.py`) are NOT covered by REUSE.toml — every new `.py` file needs inline
  `# SPDX-FileCopyrightText:` + `# SPDX-License-Identifier:` headers or REUSE CI fails.
<!-- REUSE-IgnoreEnd -->
- Python tool conventions: guard GDAL/heavy imports with `try/except ImportError` so `--help` and
  pytest unit tests work without the system package installed; unit-test pure-Python logic (no I/O,
  no GDAL) in `tests/test_<tool>.py`; add the system package + `python3-pytest` to the Ubuntu CI
  install step, plus both a `pytest` step and a GDAL synthetic-fixture smoke step (Ubuntu only).
- All code must compile on Windows (MSVC 2026), Linux (GCC/Clang), macOS (Apple Clang)
- `CMAKE_COMPILE_WARNING_AS_ERROR=ON` in debug builds — fix all warnings
- **Namespace**: every native type lives in `namespace fl`. New files in `engine/`, `game/`,
  `server/`, `tools/` and `platform/` must be wrapped in `namespace fl {}`. Entry-point `main()`
  functions use `using namespace fl;` at file scope. Sub-namespaces follow `fl::ai` / `fl::rcon`.
  Forward declarations use `namespace fl { class Foo; }` — never a bare `class Foo;` for an `fl::`
  type.
- **Do not edit `CHANGELOG.md` in a feature PR.** It is generated from conventional-commit subjects
  by `git-cliff` at release time (#1123) — a hand-written entry means every concurrent PR conflicts
  on the same list. **Your commit subject is the changelog entry**, so write it as a statement to a
  player or an operator, not a note to yourself.

## Layering — enforced at configure time

`cmake/layering.cmake` runs `fl_assert_layering()`, armed from the root `CMakeLists.txt` via
`cmake_language(DEFER CALL ...)` so it fires after every `add_subdirectory()`. It walks
`LINK_LIBRARIES`/`INTERFACE_LINK_LIBRARIES` transitively (aliases resolved, `$<LINK_ONLY:...>`
stripped) and **fails configure with the offending link chain** on four rules:

1. No `engine-*` target reaches a client/transport backend (platform-sdl3/vulkan/openal/enet/gns/net,
   SDL3::\*, Vulkan::\*, OpenAL::\*, enet6, GameNetworkingSockets::\*).
2. `engine-protocol` links only the stdlib and `Threads::Threads` — checked by
   `_fl_assert_links_only()`.
3. `fl-server` reaches no client backend.
4. The game binary and `fl-server` link the `platform-net` facade only, never `platform-enet`/
   `platform-gns` directly. (`net_check` and `bot_swarm` are exempt — deliberate enet6 instruments.)

It self-tests its own graph walker against synthetic `fl-layering-selftest-*` targets on every
configure. **A new engine target that links a backend failing configure is intentional**, not a bug
to work around. Policy write-up: architecture.md → "Module Boundary Policy".

## Traps worth knowing before you edit

- **Test mocks derive from a shared Null base**, so a new pure virtual on a HAL interface is a
  one-line default in that header rather than an edit to every mock: `tests/mock_content.h`
  (`NullContentPack`), `tests/mock_network.h` (`NullNetwork`/`TrackingNetwork`), `tests/mock_hal.h`,
  `tests/mock_gui.h`, `tests/mock_http.h`. Add the default there first. `BorrowedPack` in
  `test_content_system.cpp` is a deliberate non-owning forwarding proxy, so it still tracks the
  interface.
- **Non-pure virtuals with defaults** are how `IRenderer` grew `createTextureArray`,
  `setTerrainBiomeTextures` and `captureScreenshot` without touching a single mock. Prefer that shape
  for an optional backend capability.
- **`engine-entity` deliberately does not link `engine-sensor`** — the observed does not depend on
  the observer. Sensor types are forward-declared in `AiTickContext.h`.
- **A null pointer in `AiTickContext` is NORMATIVE**: it means "not evaluated here", not "empty". A
  null contact table keeps ground-truth behaviour; an *empty* one means the sensors ran and saw
  nothing.
- **`fl-server` runs with a null renderer.** Anything you add to the render path must tolerate it.
- **Lua is always built from source as C++** (`cmake/dependencies.cmake`). Compiled as C it raises
  errors by `longjmp`, which cannot cross a C++ frame safely. Consequence: a TU including `<lua.h>`
  must **not** wrap it in `extern "C"` and must not use `lua.hpp`. There is deliberately no
  system-Lua path.
- **Apple Clang has no floating-point `std::from_chars`** — use `strtod` for doubles.
- **`--screenshot <path>`** (plus `tools/visual_check.{sh,ps1}`) is the reliable in-engine
  visual-verification path; the renderer is not exercised on a GPU in CI.

## Key documents

`docs/` is the published site (<https://fighterslegacy.org/fighters-legacy/>). `mkdocs build
--strict` enforces nav membership, so an orphaned page fails the build, and `tools/docs_drift.py`
diffs six documented surfaces against the code **in both directions** — config keys, message ids,
Lua names, admin commands, the tool list and input actions. If you change one of those surfaces,
the paired document is not optional.

| Area | Document |
|---|---|
| Layering, decision records, content packs, screens, terrain | [architecture.md](docs/developer/architecture.md) |
| Build prerequisites, presets, sanitizers, coverage, fuzzing, CI | [development.md](docs/developer/development.md) |
| Renderer passes, GPU layouts, camera-relative invariant, HUD | [rendering.md](docs/developer/rendering.md) |
| Audio managers, builtin PCM, playlists | [audio.md](docs/developer/audio.md) |
| Wire protocol — every message, field offsets, versioning | [network-protocol.md](docs/developer/network-protocol.md) |
| Replay `.flrep` format and its version rules | [replay-format.md](docs/developer/replay-format.md) |
| Agentic AI: provider seam, MCP surface, degradation model | [ai-architecture.md](docs/developer/ai-architecture.md) |
| Voice comms · haptics · GNS transport backend | [voice.md](docs/developer/voice.md) · [haptics.md](docs/developer/haptics.md) · [gns-backend.md](docs/developer/gns-backend.md) |
| Tools, debug console, load testing, demo recording | [tools.md](docs/developer/tools.md) · [debug-console.md](docs/developer/debug-console.md) · [load-testing.md](docs/developer/load-testing.md) · [demo-recording.md](docs/developer/demo-recording.md) |
| Phase schedule + initiative tables | [roadmap.md](docs/roadmap.md) |
| Issue types, epics, labels, the project board | [project-management.md](docs/developer/project-management.md) |
| Spike resolutions (sharding, physics LOD, quantization, congestion, job system, latency, AI provider, distribution) | [decisions/](docs/developer/decisions/) |
| Content-pack asset formats — the authority for mod authors | [modding/formats.md](docs/modding/formats.md) |
| Player key map (drift-checked against `InputAction.h`) | [user-guide/controls.md](docs/user-guide/controls.md) |
| Decision-making and the RFC process | [GOVERNANCE.md](GOVERNANCE.md) |
