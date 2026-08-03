# Developer Guide

For people working on the engine, the server or the tools.

Start with [Development](development.md) to get a build, then [Architecture](architecture.md) for
the layering rules every change has to respect.

## Building and contributing

| Page | What it covers |
|---|---|
| [Development](development.md) | Prerequisites per platform, CMake presets, tests, coverage, fuzzing |
| [Project management](project-management.md) | Issue types, epics, labels, milestones, and the release process |
| [Roadmap](../roadmap.md) | Phase schedule, critical path, per-phase acceptance criteria |
| [Design](design.md) · [Prior art](prior-art.md) | The product's pillars, and the landscape it sits in |
| [References](references.md) | Upstream documentation for every dependency |

## Engine internals

| Page | What it covers |
|---|---|
| [Architecture](architecture.md) | Layered model, module-boundary policy, content-pack system |
| [Network protocol](network-protocol.md) | The wire format: message table, quantized records, extension tags |
| [Replay format](replay-format.md) | `.flrep` — versioning rules for a format that outlives its build |
| [AI architecture](ai-architecture.md) | Provider seam, world-state API, event stream, agent surface |
| [GNS backend](gns-backend.md) | GameNetworkingSockets integration notes |
| [Voice](voice.md) | Radio-net model, relay, jitter buffer |
| [Haptics](haptics.md) | The `IInput` haptic API and the rumble event catalogue |
| [Rendering](rendering.md) | Pass order, GPU layouts, the camera-relative invariant, HUD, quality tiers |
| [Audio](audio.md) | SFX/music/warning-tone managers, byte-stable builtin PCM, headless testing |

## Tools and instruments

| Page | What it covers |
|---|---|
| [Tools](tools.md) | Every CLI binary and script in `tools/` |
| [Debug console](debug-console.md) | Console commands and the developer key map |
| [Debug console](debug-console.md) | Console commands, the performance overlay, and visual verification |
| [Load testing](load-testing.md) | `bot_swarm`, the scale gate, the reference environment |
| [Demo recording](demo-recording.md) | Headless cinematic capture for phase demos |

## Decision records

[Frozen spike resolutions](decisions/index.md) — why things are the way they are. Not maintained
against current behaviour, deliberately.
