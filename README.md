# Fighters Legacy

[![CI](https://github.com/fighters-legacy/fighters-legacy/actions/workflows/ci.yml/badge.svg)](https://github.com/fighters-legacy/fighters-legacy/actions/workflows/ci.yml)
[![Coverage](https://codecov.io/gh/fighters-legacy/fighters-legacy/branch/main/graph/badge.svg)](https://codecov.io/gh/fighters-legacy/fighters-legacy)
[![CodeQL](https://github.com/fighters-legacy/fighters-legacy/actions/workflows/codeql.yml/badge.svg)](https://github.com/fighters-legacy/fighters-legacy/actions/workflows/codeql.yml)
[![REUSE status](https://api.reuse.software/badge/github.com/fighters-legacy/fighters-legacy)](https://api.reuse.software/info/github.com/fighters-legacy/fighters-legacy)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

A **general-purpose combat flight sim engine** with a first-class mod and plugin
system, inspired by Jane's Fighters Anthology (1998). Runs natively on Windows 10/11,
Linux, and macOS. All game content is delivered through content packs —
every asset source is a mod or plugin.

A rich single-player experience and **large-scale multiplayer are co-equal goals**: the
architecture targets **128+ simultaneous players** on self-hosted, server-authoritative
dedicated servers, with pluggable identity, anti-cheat, and a Kubernetes/OpenShift operator
for clustered fleets.

> NOTE: The project is currently in active development toward an initial public release.

## Documentation

| Document | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Layered model, locked decisions, content pack architecture |
| [docs/design.md](docs/design.md) | Gameplay design pillars, lifted FA constraints |
| [docs/modding/formats.md](docs/modding/formats.md) | Native asset format specs (glTF, TOML, YAML, Lua) |
| [docs/modding/localization.md](docs/modding/localization.md) | Translator guide: key scheme, TOML layout, plural forms, mod locale |
| [docs/network-protocol.md](docs/network-protocol.md) | Wire protocol: fl-server ↔ client message format, channel assignments, scalability notes |
| [docs/snapshot-quantization.md](docs/snapshot-quantization.md) | Quantized/bit-packed snapshot codec: frame-origin encoding, smallest-three quaternion, bit budget, byte-determinism |
| [docs/load-testing.md](docs/load-testing.md) | bot_swarm load generator: client metrics + authoritative server tick-budget block, flight patterns, scale-gate targets, the CI perf/soak scale gate (PR + scheduled tiers), ceiling-characterisation runbook |
| [docs/server-job-system-design.md](docs/server-job-system-design.md) | Data-parallel sim tick: engine-job worker pool, two-phase parallel onTick, serial-equivalence, `sim_worker_threads` |
| [docs/congestion-control-design.md](docs/congestion-control-design.md) | Adaptive per-client send-rate / congestion response: AIMD controller, ENet-RTT anti-feedback signal, send-rate + byte-budget levers, `[world] congestion_*` |
| [docs/roadmap.md](docs/roadmap.md) | Schedule, critical path, acceptance criteria, the Multiplayer-at-Scale initiative |
| [docs/distribution.md](docs/distribution.md) | Distribution channels (incl. fl-server container/Helm/operator), monetization strategy |
| [docs/development.md](docs/development.md) | Build prerequisites per platform (C++ engine + Go services) |
| [docs/references.md](docs/references.md) | Technology reference index: upstream docs for every engine dependency and tool |
| [docs/project-management.md](docs/project-management.md) | How work is managed: issue types, epics/sub-issues, labels, milestones, the Project board |
| [GOVERNANCE.md](GOVERNANCE.md) | Decision-making, RFC process, and decision records |

### Related repositories

The engine and game live here. The 128+ multiplayer re-target introduces companion
repositories under the [`fighters-legacy`](https://github.com/fighters-legacy) org:

| Repository | Role |
|---|---|
| `fl-base-pack` | The starter content pack (aircraft, terrain, missions, audio, AI) |
| `fl-account` *(planned, Go)* | Pluggable identity / account service (self-hostable) |
| `fl-review` *(planned, Go)* | Offline anti-cheat batch-analysis service |
| `fl-operator` *(planned, Go)* | Kubernetes / OpenShift operator + Helm chart for clustered fleets |

## Roadmap

Development is tracked through [GitHub milestones](https://github.com/fighters-legacy/fighters-legacy/milestones).

| Phase | Description |
|---|---|
| 1 — Engine Foundation ✓ | HAL, Vulkan, SDL3, OpenAL, ENet, content system, CI/CD |
| 2 — Modern-Particles Engine ✓ | Game loop, flight model, AI, networking, renderer, spherical-Earth world model |
| [3 — Engine Systems](https://github.com/fighters-legacy/fighters-legacy/milestone/8) | Spatial partitioning, interest management, AI framework, bindings, quality settings, pilot profiles, scaling seams (transport, sim job system, wire quantization, load harness) |
| [4 — Content & Gameplay](https://github.com/fighters-legacy/fighters-legacy/milestone/9) | fl-base-pack content, radar/weapons/EW + sensor framework, AI, missions, campaign, MP gameplay framework, advanced vehicle models |
| 5 — Multiplayer at Scale & Live Services | Server-side identity/auth, anti-cheat, persistence, ops/observability, k8s/OpenShift operator |
| [6 — UI Layer & Tooling](https://github.com/fighters-legacy/fighters-legacy/milestone/4) | IGui HAL + Dear ImGui backend, in-game mission editor, welcome screen |
| [7 — Platform Release](https://github.com/fighters-legacy/fighters-legacy/milestone/5) | macOS/Linux/Windows packages, Flathub, fl-server container, crash reporting |
| [8 — OpenGL & Alternative Renderers](https://github.com/fighters-legacy/fighters-legacy/milestone/7) | OpenGL 4.1 Core backend, headless/software renderer for CI, voice chat |
| [9 — Modding Platform](https://github.com/fighters-legacy/fighters-legacy/milestone/6) | GPG verification, subprocess isolation, in-game mod browser, community content distribution |

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

Copyright © 2026 MKZ Systems LLC.

**Fighters Legacy™** is a trademark of MKZ Systems LLC. The GPL covers the code, not the
name or logo — see [TRADEMARK.md](https://github.com/fighters-legacy/.github/blob/main/TRADEMARK.md) for what community redistributions may and
may not call themselves.
