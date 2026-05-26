# Fighters Legacy

[![CI](https://github.com/jomkz/fighters-legacy/actions/workflows/ci.yml/badge.svg)](https://github.com/jomkz/fighters-legacy/actions/workflows/ci.yml)
[![Coverage](https://codecov.io/gh/jomkz/fighters-legacy/branch/main/graph/badge.svg)](https://codecov.io/gh/jomkz/fighters-legacy)
[![CodeQL](https://github.com/jomkz/fighters-legacy/actions/workflows/codeql.yml/badge.svg)](https://github.com/jomkz/fighters-legacy/actions/workflows/codeql.yml)
[![REUSE status](https://api.reuse.software/badge/github.com/jomkz/fighters-legacy)](https://api.reuse.software/info/github.com/jomkz/fighters-legacy)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

A **general-purpose combat flight sim engine** with a first-class mod and plugin
system, inspired by Jane's Fighters Anthology (1998). Runs natively on Windows 10/11,
Linux, and macOS. All game content is delivered through the `IContentPack` interface —
every asset source is a mod or plugin identical in status to
any community contribution.

## Documentation

| Document | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Layered model, locked decisions, content pack architecture |
| [docs/design.md](docs/design.md) | Gameplay design pillars, lifted FA constraints |
| [docs/modding/formats.md](docs/modding/formats.md) | Native asset format specs (glTF, TOML, YAML, Lua) |
| [docs/modding/localization.md](docs/modding/localization.md) | Translator guide: key scheme, TOML layout, plural forms, mod locale |
| [docs/roadmap.md](docs/roadmap.md) | Schedule, critical path, acceptance criteria |
| [docs/distribution.md](docs/distribution.md) | Distribution channels, monetization strategy |
| [docs/development.md](docs/development.md) | Build prerequisites per platform |
| [GOVERNANCE.md](GOVERNANCE.md) | Decision-making and RFC process |

## Roadmap

Development is tracked through [GitHub milestones](https://github.com/jomkz/fighters-legacy/milestones).

| Phase | Description |
|---|---|
| [1 — Engine Foundation](https://github.com/jomkz/fighters-legacy/milestone/1) | HAL, Vulkan, SDL3, OpenAL, ENet, content system, CI/CD |
| [2 — Modern-Particles Engine](https://github.com/jomkz/fighters-legacy/milestone/2) | Game loop, flight model, AI, networking, renderer |
| [3 — Classic/Parity Mode](https://github.com/jomkz/fighters-legacy/milestone/3) | SH bytecode interpreter, software rasterizer |
| [4 — In-Game Mission Editor](https://github.com/jomkz/fighters-legacy/milestone/4) | In-game mission editor |
| [5 — Linux/macOS Release](https://github.com/jomkz/fighters-legacy/milestone/5) | MoltenVK, Flathub, official binaries |
| [6 — Native Formats + Free Pack](https://github.com/jomkz/fighters-legacy/milestone/6) | Open asset toolchain, mod browser, community base pack |

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
