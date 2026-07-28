# Modding Guide

For content authors: aircraft, missions, terrain, art, audio and AI behaviour.

A mod is a directory with a [`manifest.toml`](manifest-reference.md) in it. Everything else is
optional, and the engine ships builtin fallbacks for every asset class, so a pack that provides
one aircraft and nothing else is valid and loads cleanly.

## Start here

| Page | What it covers |
|---|---|
| [Pack manifest](manifest-reference.md) | `manifest.toml`: id, namespace, version, priority, dependencies |
| [Asset formats](formats.md) | The canonical reference for every asset format a pack can contain |

## Authoring

| Page | What it covers |
|---|---|
| [3D models](3d-models.md) | glTF conventions, coordinate system, LODs, damage meshes, animation channels |
| [Textures](textures.md) | Source vs artifact, channel layout, Basis Universal compression |
| [Liveries](liveries.md) | Livery TOML and the two-vocabulary rule |
| [Flight models](flight-model.md) | Every field of the aircraft aerodynamics TOML |
| [Weapons & sensors](weapons-sensors.md) | Radar modes, IR, RWR, IFF, datalink, seekers, countermeasures |
| [Missions](missions.md) | Mission YAML: coalitions, objects, triggers, objectives, airspace zones |
| [Game modes](game-modes.md) | Teams, scoring, respawn rules, match lifecycle |
| [AI scripting](ai.md) | The Lua API: controllers, the `world` module, and the sandbox rules |
| [Satellite terrain](satellite-terrain.md) | Generating imagery tiles from open satellite data |
| [Localization](localization.md) | Translating the game and pack strings |

## Rules that bind

- **[Aircraft likeness policy](../legal/aircraft-likeness.md)** — real types may be modelled, but
  sources must be public-domain and every aircraft needs a `SOURCES.md`. Read this before starting
  an aircraft, not after.
- **Validate before you publish.** Every asset class has a `validate-*` tool
  ([tools](../developer/tools.md)), and the pack CI gates run them. A file that fails its validator
  will not load.
