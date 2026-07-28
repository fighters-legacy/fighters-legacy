# Fighters Legacy documentation

A general-purpose combat flight simulator engine with a first-class mod and plugin system.

**Start with the guide that matches what you are doing.** Every page belongs to exactly one of
these four. Nothing lives in two places, so when a page here does not answer your question, one of
the other three owns it.

| I want to… | Guide |
|---|---|
| **Play the game** — install it, fly, fight, fly with friends | [User Guide](user-guide/index.md) |
| **Run a server** — configure, secure, monitor and administer it | [Server Ops Guide](server-ops/index.md) |
| **Make content** — aircraft, missions, terrain, liveries, AI scripts | [Modding Guide](modding/index.md) |
| **Work on the engine** — build it, understand it, contribute to it | [Developer Guide](developer/index.md) |

---

## User Guide

For players. No configuration files, no build tools.

| Page | What it covers |
|---|---|
| [Installation](user-guide/installation.md) | Getting the game running, system requirements, content packs |
| [Quick start](user-guide/quickstart.md) | Your first flight, in about five minutes |
| [Controls](user-guide/controls.md) | The key map, gamepads, HOTAS, and the camera modes |
| [Wingman & voice](user-guide/voice-and-wingman.md) | Ordering your flight by menu, chat or voice, and talking on the radio |
| [Replays & photo mode](user-guide/replays-and-photo-mode.md) | Watching a match back, scrubbing it, and taking pictures |
| [Multiplayer](user-guide/multiplayer.md) | Finding and joining servers, and what to expect from the netcode |
| [Linux gamepad setup](user-guide/gamepad-linux.md) | Controllers, HOTAS and joysticks on Linux |

## Server Ops Guide

For anyone running a dedicated server.

| Page | What it covers |
|---|---|
| [Server configuration](server-ops/server-config.md) | Every `server.toml` key, every CLI flag, and the admin command reference |
| [Admin API](server-ops/admin-api.md) | REST endpoints, bearer-token auth, and the health probe |
| [Agent surface (MCP)](server-ops/mcp-agent-surface.md) | Exposing the server to AI agents, and the security model that bounds them |
| [Metrics](server-ops/metrics.md) | What `--metrics-json` reports, and how to read it |
| [Lobby API](server-ops/lobby-api.md) | Registering with a lobby so players can find you |

## Modding Guide

For content and mission authors.

| Page | What it covers |
|---|---|
| [Asset formats](modding/formats.md) | The canonical reference for every pack asset format |
| [Pack manifest](modding/manifest-reference.md) | `manifest.toml` — the file that makes a directory a mod |
| [Missions](modding/missions.md) | Mission YAML: objects, triggers, objectives, airspace zones |
| [AI scripting](modding/ai.md) | The Lua API — controllers, the `world` module, the sandbox rules |
| [Flight models](modding/flight-model.md) | Aerodynamics TOML, field by field |
| [3D models](modding/3d-models.md) | glTF conventions, LODs, damage meshes, animation channels |
| [Textures](modding/textures.md) · [Liveries](modding/liveries.md) | Authoring and compressing pack art |
| [Weapons & sensors](modding/weapons-sensors.md) | Radar, IR, RWR, seekers, countermeasures |
| [Game modes](modding/game-modes.md) | Scoring, teams, respawn and match rules |
| [Satellite terrain](modding/satellite-terrain.md) | Generating imagery tiles from open data |
| [Localization](modding/localization.md) | Translating the game |
| [Aircraft likeness policy](legal/aircraft-likeness.md) | What you may model, and which sources you may use |

## Developer Guide

For people working on the engine itself.

| Page | What it covers |
|---|---|
| [Development](developer/development.md) | Prerequisites, presets, building, testing, fuzzing |
| [Architecture](developer/architecture.md) | Layering, module boundaries, the content-pack system |
| [Design](developer/design.md) · [Roadmap](roadmap.md) · [Prior art](developer/prior-art.md) | Where the project is going, and why |
| [Project management](developer/project-management.md) | Issues, epics, labels, milestones, the release process |
| [Network protocol](developer/network-protocol.md) | The wire format — message table, records, extension tags |
| [Replay format](developer/replay-format.md) | `.flrep` — the first format that outlives its build |
| [AI architecture](developer/ai-architecture.md) | The provider seam, world-state API and agent surface |
| [Tools](developer/tools.md) | Every CLI tool in `tools/`, and what each is for |
| [Debug console](developer/debug-console.md) | Developer console commands and the sandbox key map |
| [Voice](developer/voice.md) · [Haptics](developer/haptics.md) · [Rendering](developer/rendering.md) | Subsystem references |
| [Load testing](developer/load-testing.md) · [Demo recording](developer/demo-recording.md) | Tooling guides |
| [GNS backend](developer/gns-backend.md) · [References](developer/references.md) | Transport internals, and upstream docs per dependency |
| **[Decision records](developer/decisions/index.md)** | Frozen spike resolutions — history, not guides |

---

## Project documents

[Contributing](https://github.com/fighters-legacy/fighters-legacy/blob/main/CONTRIBUTING.md) ·
[Governance](https://github.com/fighters-legacy/fighters-legacy/blob/main/GOVERNANCE.md) ·
[Support](https://github.com/fighters-legacy/fighters-legacy/blob/main/SUPPORT.md) ·
[Changelog](https://github.com/fighters-legacy/fighters-legacy/blob/main/CHANGELOG.md) ·
[Code of Conduct](https://github.com/fighters-legacy/.github/blob/main/CODE_OF_CONDUCT.md) ·
[Security policy](https://github.com/fighters-legacy/.github/blob/main/SECURITY.md)
