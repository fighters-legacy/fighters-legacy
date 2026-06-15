# Fighters Legacy

[![CI](https://github.com/jomkz/fighters-legacy/actions/workflows/ci.yml/badge.svg)](https://github.com/jomkz/fighters-legacy/actions/workflows/ci.yml)
[![Coverage](https://codecov.io/gh/jomkz/fighters-legacy/branch/main/graph/badge.svg)](https://codecov.io/gh/jomkz/fighters-legacy)
[![CodeQL](https://github.com/jomkz/fighters-legacy/actions/workflows/codeql.yml/badge.svg)](https://github.com/jomkz/fighters-legacy/actions/workflows/codeql.yml)
[![REUSE status](https://api.reuse.software/badge/github.com/jomkz/fighters-legacy)](https://api.reuse.software/info/github.com/jomkz/fighters-legacy)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

A **general-purpose combat flight sim engine** with a first-class mod and plugin
system, inspired by Jane's Fighters Anthology (1998). Runs natively on Windows 10/11,
Linux, and macOS. All game content is delivered through content packs —
every asset source is a mod or plugin.

> NOTE: The project is currently in active development toward an initial public release.

## Documentation

| Document | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Layered model, locked decisions, content pack architecture |
| [docs/design.md](docs/design.md) | Gameplay design pillars, lifted FA constraints |
| [docs/modding/formats.md](docs/modding/formats.md) | Native asset format specs (glTF, TOML, YAML, Lua) |
| [docs/modding/localization.md](docs/modding/localization.md) | Translator guide: key scheme, TOML layout, plural forms, mod locale |
| [docs/network-protocol.md](docs/network-protocol.md) | Wire protocol: fl-server ↔ client message format, channel assignments, scalability notes |
| [docs/roadmap.md](docs/roadmap.md) | Schedule, critical path, acceptance criteria |
| [docs/distribution.md](docs/distribution.md) | Distribution channels, monetization strategy |
| [docs/development.md](docs/development.md) | Build prerequisites per platform |
| [docs/references.md](docs/references.md) | Technology reference index: upstream docs for every engine dependency and tool |
| [GOVERNANCE.md](GOVERNANCE.md) | Decision-making and RFC process |

## Roadmap

Development is tracked through [GitHub milestones](https://github.com/jomkz/fighters-legacy/milestones).

| Phase | Description |
|---|---|
| 1 — Engine Foundation ✓ | HAL, Vulkan, SDL3, OpenAL, ENet, content system, CI/CD |
| 2 — Modern-Particles Engine ✓ | Game loop, flight model, AI, networking, renderer, spherical-Earth world model |
| [3 — Engine Systems](https://github.com/jomkz/fighters-legacy/milestone/8) | Spatial partitioning, interest management, AI framework, bindings, quality settings, pilot profiles |
| [4 — Content & Gameplay](https://github.com/jomkz/fighters-legacy/milestone/9) | fl-base-pack content, radar/weapons/EW, AI, missions, campaign, multiplayer, advanced vehicle models |
| [5 — UI Layer & Tooling](https://github.com/jomkz/fighters-legacy/milestone/4) | IGui HAL + Dear ImGui backend, in-game mission editor, welcome screen |
| [6 — Platform Release](https://github.com/jomkz/fighters-legacy/milestone/5) | macOS/Linux/Windows packages, Flathub, fl-server container, crash reporting |
| [7 — OpenGL & Alternative Renderers](https://github.com/jomkz/fighters-legacy/milestone/7) | OpenGL 4.1 Core backend, headless/software renderer for CI, voice chat |
| [8 — Modding Platform](https://github.com/jomkz/fighters-legacy/milestone/6) | GPG verification, subprocess isolation, in-game mod browser, community content distribution |

See [docs/roadmap.md](docs/roadmap.md) for the schedule, critical path, and
per-phase acceptance criteria.

## License

The engine and all code in this repository are licensed under **GPL v3**. Anyone who
distributes a modified version of the engine must publish the source under the same
terms.

| Artifact | GPL obligation? | Reason |
|---|---|---|
| Lua AI scripts (`.lua`) | No | Scripts run in a sandboxed interpreter |
| Asset files (glTF, TOML, YAML, OGG, PNG, KTX2) | No | Data, not code |
| Mission and campaign YAML | No | Data |
| Compiled content pack (`.dll` / `.so`) | Yes, unless exception granted | Linked directly against engine code |

`IContentPack.h` carries a **GPL linking exception** permitting content pack authors to
link against the interface without their pack being subject to GPL v3. This covers most
mod authors — see [LICENSES/](LICENSES/) for the full text.
