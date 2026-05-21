# Fighters Legacy — Master Plan

## Context

Jane's Fighters Anthology (1998) runs only on legacy Windows and requires compatibility
shims for audio and display. The goal is a clean-room reimplementation that loads game
assets, modernizes the engine (Vulkan renderer, GPU particles, real networking, high
resolution), and runs natively on Windows 10/11, Linux, and macOS.

Beyond reimplementing FA, Fighters Legacy is designed as a **general-purpose combat flight
sim engine** with a first-class mod system. Original FA asset support is delivered as a
content plugin, identical in status to any user mod. People who do not own FA can play
with a community-contributed free base pack using fully open asset formats.

The RE corpus in `docs/fa/` (fighters-toolkit) is the foundation: 53 file formats
documented, game loop / physics / renderer / networking all traced from FA.EXE using
FA.SMS symbols.

**FA content translates inward, never outward.** The engine core is unaware of FA's
formats, limitations, or design decisions. All FA assets are translated into engine-native
data structures by the fa-content bridge before the engine ever sees them. No FA format,
naming convention, or design constraint is permitted to leak into the engine layer.

## Locked Architectural Decisions

| Concern | Choice | Rationale |
|---|---|---|
| Rendering | Vulkan + MoltenVK | One API everywhere; MoltenVK → Metal on Apple Silicon |
| Windowing / input | SDL3 | Wayland + modern controller support; long-term path |
| Audio | OpenAL Soft | Positional 3D audio; native music in OGG; no MIDI dependency in engine core |
| Network transport | ENet (reliable UDP) | Reliable + unreliable channels; congestion control; cross-platform |
| Build system | CMake 3.25+ | Cross-platform from day one |
| Asset library | `ft_lib` as git submodule | Reuses all 22 codecs; only used inside the FA plugin |
| Engine repo | `fighters-legacy` (this repo) | Separate from fighters-toolkit |
| ft-gui future | Port to SDL3 + Vulkan | After engine HAL is stable (Phase 4) |
| Content system | Plugin / content-pack architecture | FA assets = one plugin; mods = other plugins; engine core has zero FA dependency |
| Native 3D models | glTF 2.0 | Royalty-free; Blender export; industry standard |
| Native textures | PNG (source) + KTX2/DDS (GPU) | Mipmaps, BC compression; toolchain converts PNG → KTX2 at pack time |
| Native audio | OGG Vorbis / Opus | Open, compressed, widely supported |
| Native flight model | TOML | Human-readable, structured, easily diffable |
| Native missions | YAML | Human-readable, tool-friendly |
| Native campaigns | YAML | Arbitrary theater graph; no FA 6-theater limit |
| Native terrain | Streaming heightmap chunks + JSON | No tile-count cap; supports large theaters |
| Native AI scripts | Lua 5.4 | Embeddable, sandbox-able, moddable; FA goto-scripts translated at import |
| Multiplayer topology | `fl-server` dedicated binary + `fl-lobby` REST service | Server-authoritative; lifts FA's P2P player-count cap; self-hostable |
| Entity system | Dynamic pool, no hard caps | Lifts FA's 799/899 object limits |
| License | GPL v3 | Engine modifications must stay open source; protects community investment |
| Hosting | GitHub, public repository | Unlimited Actions CI on public repos; GitHub Free sufficient; cancel Team license |

## License

The engine and all code in this repository are licensed under **GPL v3**. Anyone who
distributes a modified version of the engine must publish the source under the same
terms. This protects the community's investment — no company can take the engine,
improve it privately, and ship a closed product without contributing back.

**What GPL v3 means for mod and content pack authors:**

| Artifact | GPL obligation? | Reason |
|---|---|---|
| Lua AI scripts (`.lua`) | No | Scripts run in a sandboxed interpreter; not compiled against the engine |
| Asset files (glTF, TOML, YAML, OGG, PNG, KTX2) | No | Data, not code |
| Mission and campaign YAML | No | Data |
| Compiled content pack (`.dll` / `.so` implementing `IContentPack`) | Yes, unless exception granted | Linked directly against engine code |
| fa-content bridge | GPL v3 (in this repo) | Ships as part of this project |

The compiled content pack case is the only friction point. Most mods are Lua + assets
and are completely unaffected. For compiled packs, `IContentPack.h` will carry a
**GPL linking exception** — similar to the GCC Runtime Library Exception — permitting
content pack authors to link against the interface without their pack being subject to
GPL v3. This will be added before the first public release so the community has clarity
from day one.

## Gameplay Design Pillars

These are the non-negotiable design values that resolve every ambiguous feature decision.

- **Arcade-to-sim balance**: Closer to Ace Combat / Project Wingman than DCS World. Physics
  are simplified but feel consequential. Stalls, G-effects, and energy management matter;
  startup checklists, instrument-only navigation, and systems management do not.
- **Depth through variety, not complexity**: Replayability comes from varied missions,
  wingman decisions, loadout tradeoffs, and campaign outcomes — not from the number of
  cockpit buttons or procedures.
- **FA-style controls**: Many key bindings, each with a clear and immediate effect.
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

## Lifted Constraints

FA's design choices that are **explicitly not carried forward**. The fa-content bridge
maps FA's limited formats onto the engine's richer structures; the engine itself has no
knowledge of these limits.

| FA Limitation | FA Value | Fighters Legacy |
|---|---|---|
| Theater size | 32×32 tiles (~59 km/side) | Arbitrary; streaming terrain chunks |
| Object pool | 300,000 bytes; hard caps 799 / 899 | Dynamic allocation; soft limits tunable per server |
| Multiplayer topology | P2P master/slave (IPX/serial/modem) | Dedicated `fl-server`; optional `fl-lobby` matchmaking |
| Multiplayer players | ~8 (P2P bandwidth limit) | 32+ (server-authoritative) |
| Campaign structure | 6 fixed theaters, linear progression | Arbitrary YAML graph; branching, nested objectives, any count |
| AI scripting | Goto-script (LABEL/CONDITION/ACTION); one script per entity | Lua 5.4; full scripting API; multiple concurrent behaviors |
| Score / rank tiers | Fixed PLT binary offsets | Data-driven; TOML-defined rank tables |
| Aircraft count | Fixed BRF enumeration | Unlimited; all content pack entries |
| Weapon hardpoints | 10 per aircraft (BRF limit) | Configurable per aircraft TOML |
| Audio sample rates | 5 kHz / 8 kHz / 11 kHz raw PCM | OGG at any rate; FA rates up-sampled on import |
| Render resolution | 640×480 / 800×600 / 1024×768 | Any resolution; windowed or fullscreen |

## Content Pack Architecture

This is the central design decision that affects every other phase. **The engine core
never imports or calls `ft_lib` directly.** All asset access goes through an
`IContentPack` interface.

### IContentPack Interface

```
engine/content/IContentPack.h
    name(), version(), priority()
    hasAsset(name, AssetType) → bool
    loadMesh(name)        → MeshData     (glTF-compatible vertex/index buffers)
    loadTexture(name)     → TextureData  (RGBA or BC-compressed)
    loadAudio(name)       → AudioBuffer  (PCM or streaming OGG)
    loadFlightModel(name) → FlightModel  (struct populated from TOML or translated BRF)
    loadMission(name)     → MissionData  (struct populated from YAML or translated .M)
    loadTerrain(name)     → TerrainData  (heightmap + surface classes)
    loadAIScript(name)    → AIScript     (Lua source; FA goto-script translated by bridge)
    listAssets(AssetType) → vector<string>
```

### Mod Loader

```
engine/content/ModLoader.cpp
    scanModsDirectory("mods/")
    loadManifest("mods/<name>/manifest.toml")
    buildContentStack()   — sorted by priority; higher-priority packs override lower
    resolveAsset(name, type) — walk stack until found
```

### Mod Manifest Format (TOML)

```toml
[mod]
name        = "Fighters Anthology Content Bridge"
id          = "fa-content"
version     = "1.0.0"
engine-api  = "1.0"      # minimum engine API version required; shows warning if newer
priority    = 100        # higher = loads first; user mods typically 0–50
requires-fa = true       # engine prompts for FA install path on first run

# General mod dependency declaration; mod browser auto-installs missing deps
depends     = []         # e.g. ["base-weapons-pack@1.2", "community-factions@0.5"]
```

The mod loader resolves `depends` entries before activating the mod: checks local installs
first, then queries the community index, then falls back to any source URL in the index
entry. A version range (`@1.2` = any 1.x ≥ 1.2) is supported. Circular dependencies are
detected and reported at load time. The `engine-api` field is checked against the running
engine version; a mismatch shows a non-blocking warning ("This mod was built for engine
API 1.0; you are running 1.3 — it may work but is unsupported").

**Mod API versioning policy**: `engine-api` uses semantic versioning. Minor bumps (1.0 →
1.1) are additive — new API functions only; existing mods continue to work unmodified.
Major bumps (1.x → 2.0) may remove or rename API surfaces; mods must update their
`engine-api` field and adapt. When a major bump ships, the engine carries a one-release
compatibility shim for the previous major version with deprecation warnings, giving mod
authors one release cycle to migrate. The migration guide for each major bump is
published in `docs/modding/api-changelog.md`. Mods pinning an old major version are
disabled at startup (not silently broken) with a clear error and a link to the changelog.

**Compiled mod trust model**: Lua AI scripts run in a restricted sandbox with no
filesystem, network, or FFI access. Compiled content pack `.dll`/`.so` files are a
different category — they execute native unmanaged code with the same privileges as the
engine process. The engine imposes no additional sandbox on compiled mods; they are
treated like any other native binary the user chooses to run. **Install compiled mods
only from authors whose source you can verify.** The community index may flag mods as
"source-available" (published source on a public repo) vs. "binary-only"; the mod
browser displays this status. The engine will never auto-install a compiled mod without
explicit user confirmation, even when joining a server that requires it.

**Engine and save version compatibility policy**:

*Client ↔ server*: the engine minor version is compatible within the same major version
(1.2 client connects to 1.3 server with a non-blocking warning; 1.x client cannot connect
to a 2.x server and receives a clear "version mismatch — update required" message).
fl-server advertises its version in the lobby registration; the server browser shows a
version badge and filters out incompatible servers by default (toggle to show all).

*Pilot saves and campaign saves*: save files are versioned with a `schema_version` field.
Minor bumps add optional fields with defaults — old saves load in new engines without
data loss. Major bumps require a migration: the engine detects an old schema on load,
runs a migration script, writes a new file, and keeps the original as `<file>.bak`.
Migration is always automatic and lossless within the same major version series; if
migration is not possible (too old a schema), the engine says so and refuses to corrupt
the file. No silent data loss.

### Content Pack Layout on Disk

```
mods/
    fa-content/               ← FA content bridge (shipped separately; users install)
        manifest.toml
        fa-content.dll/.so
    free-base-pack/           ← community open-content (Phase 6+)
        manifest.toml
        aircraft/             ← glTF + TOML flight models
        missions/             ← YAML mission files
        terrain/              ← heightmap PNGs + JSON
        audio/                ← OGG files
    my-reskin-mod/            ← user mod example
        manifest.toml
        aircraft/f22/
            F22.png           ← overrides FA's F22 skin texture
```

### FA Content Bridge Plugin

Located at `plugins/fa-content/` within this repo. Optional CMake target
(`-DFL_FA_CONTENT=ON`). Implements `IContentPack` using `ft_lib`:

| Request | Translation |
|---|---|
| `loadMesh("F22")` | `ft_lib sh_parse_mesh("F22.SH")` → `MeshData` |
| `loadTexture("_f22")` | `ft_lib pic_decode("_F22.PIC", pal)` → RGBA `TextureData` |
| `loadAudio("JET_11K")` | `ft_lib audio_decode("JET.11K")` → PCM → OGG `AudioBuffer` |
| `loadAudio("THEME")` | `ft_lib mus_decode("THEME.MUS")` → MIDI → FluidSynth PCM → OGG; cached |
| `loadFlightModel("F22")` | `ft_lib brf_parse("F22.PT")` → `FlightModel` struct |
| `loadMission("U01")` | `ft_lib mission_parse("U01.M")` → `MissionData` |
| `loadCampaign("UKRAINE")` | `ft_lib cam_parse("UKRAINE.CAM")` → `CampaignData` (YAML-equivalent struct) |
| `loadTerrain("UKRAINE")` | `ft_lib t2_parse("UKRAINE.T2")` → `TerrainData` |
| `loadAIScript("F")` | `ft_lib ai_parse("F.AI")` → translate goto-script → Lua compatibility wrapper |

On startup, the bridge discovers the FA installation via `FA_INSTALL_DIR` env var or
a path dialog, then mounts the LIB archives via `ft_lib ealib`.

**Translated assets are optionally cached to disk** (e.g. `cache/fa-content/F22.glb`)
to avoid re-translation on every launch.

## Native Open Asset Format Specifications

These are the engine's canonical formats. All non-FA content uses these. The FA bridge
translates FA formats into these at runtime.

### 3D Models — glTF 2.0
- Aircraft, vehicles, weapons, buildings, terrain features
- Damage states: separate glTF meshes or morph targets (`_b` suffix = battle-damaged)
- LOD variants: glTF `LOD` extension or separate files (`F22_lod0.glb`, `F22_lod1.glb`)
- Animations: glTF `animations` array (gear extend/retract, prop rotation, bay doors)
- Shadow mesh: separate `F22_shadow.glb`
- Cockpit interior: optional separate `F22_cockpit.glb`; includes camera anchor point and
  instrument panel geometry. If absent, cockpit view falls back to 2D HUD overlay only.
  Instruments are non-interactive geometry (no DCS-style clickable cockpit).
- Toolchain: Blender → glTF 2.0 export (documented in `docs/modding/3d-models.md`)

### Textures — PNG + KTX2
- Source: PNG (RGBA, any resolution)
- GPU-ready: KTX2 with BC1/BC3/BC7 compression + mipmaps generated at pack time
- Naming: `aircraft_f22.png`, `terrain_grass.png` (lowercase, snake_case)
- Palette-mapped textures from FA are converted to full RGBA at import time

### Audio — OGG Vorbis / Opus
- Sound effects: OGG at 44.1 kHz stereo or mono
- Music: OGG (pre-rendered from MIDI/FluidSynth during content pack build)
- FA's raw PCM (`.11K`/`.5K`/`.8K`) converted to OGG by the FA bridge on first access
- Streaming sources (long music tracks) use OpenAL streaming buffers

### Music Playlist — TOML
Controls which music track plays in each named game state. Content packs supply a
playlist file; the engine's music system transitions between states and crossfades tracks.
The FA bridge generates an equivalent playlist from FA's MUS files on first run.

```toml
# audio/playlist.toml
[crossfade]
duration_s = 3.0    # fade-out + fade-in overlap when switching states

[[states]]
id      = "menu"
tracks  = ["audio/music/menu_theme.ogg"]
loop    = true

[[states]]
id      = "briefing"
tracks  = ["audio/music/briefing.ogg"]
loop    = false     # plays once; silence after

[[states]]
id      = "flight_patrol"
tracks  = ["audio/music/patrol_01.ogg", "audio/music/patrol_02.ogg"]
loop    = true
shuffle = true      # randomise track order

[[states]]
id      = "flight_combat"
tracks  = ["audio/music/combat_01.ogg", "audio/music/combat_02.ogg"]
loop    = true
shuffle = false

[[states]]
id      = "mission_success"
tracks  = ["audio/music/victory.ogg"]
loop    = false

[[states]]
id      = "mission_failure"
tracks  = ["audio/music/defeat.ogg"]
loop    = false
```

State transitions are driven by engine events: `combat` triggers when an enemy radar
locks the player or a missile is fired; reverts to `patrol` after 30 s with no threat.
Lua scripts can force a state change with `world.set_music_state("flight_combat")`.
A content pack with no playlist file gets silence for all music states — sound effects
still play normally.

### Flight Model — TOML
```toml
[aircraft]
name          = "F-22 Raptor"
type          = "fighter"
mesh          = "f22"
cockpit       = "f22_hud"

[performance]
mil_thrust_lb       = 25000
ab_thrust_lb        = 35000
mil_fuel_flow_lb_hr = 8000
ab_fuel_flow_lb_hr  = 24000
fuel_capacity_lb    = 18000
one_g_stall_kts     = 120
max_speed_kts       = 1000

[aerodynamics]
g_drag_base         = 0.018
g_drag_scale        = 1.4
bank_rate           = 0.71
has_thrust_vector   = true
min_nozzle_angle    = -20
max_nozzle_angle    = 20
nozzle_slew_rate    = 5

[[hardpoints]]        # repeat per slot; no engine-imposed limit
slot = 0
type = "missile"
default = "aim120c"
```

### Weapon Data — TOML
Each weapon is a standalone TOML file. Aircraft TOML hardpoints reference weapon IDs.
The FA bridge translates FA's BRF weapon entries to equivalent weapon TOML structs.

```toml
# weapons/aim120c.toml — Active-radar air-to-air missile
[weapon]
id       = "aim120c"
name     = "AIM-120C AMRAAM"
type     = "missile"
category = "air-to-air"

[seeker]
type            = "active-radar"   # active-radar | semi-active-radar | ir | laser | optical | unguided
fov_deg         = 60
acquisition_nm  = 20               # autonomous acquisition range after fire-and-forget
fire_and_forget = true

[performance]
max_range_nm      = 30
min_range_nm      = 0.5
max_speed_kts     = 2400
motor_burn_time_s = 4.5
max_g             = 30

[warhead]
blast_radius_ft = 50
damage          = 100              # normalized 0–200; 100 kills most fighters

[countermeasures]
chaff_susceptibility = 0.4        # 0 = immune, 1 = easily defeated
notch_susceptibility = 0.6        # doppler notch effectiveness

[load]
weight_lb   = 335
drag_factor = 0.008
```

```toml
# weapons/gbu12.toml — Laser-guided bomb
[weapon]
id       = "gbu12"
name     = "GBU-12 Paveway II"
type     = "bomb"
category = "air-to-ground"

[guidance]
type                = "laser"   # laser | gps | ir | unguided
requires_designator = true      # player or wingman must lase the target

[performance]
standoff_range_ft = 15000
CEP_ft            = 8

[warhead]
blast_radius_ft = 100
damage          = 180           # heavy; destroys hardened targets

[load]
weight_lb   = 500
drag_factor = 0.020
```

```toml
# weapons/m61a2.toml — Internal gun
[weapon]
id       = "m61a2"
name     = "M61A2 Vulcan"
type     = "gun"
category = "air-to-air"

[performance]
rounds_per_min     = 6000
muzzle_vel_fps     = 3400
max_rounds         = 512
effective_range_ft = 4000

[warhead]
damage = 15                     # per hit; multiple hits required for kill
```

Aircraft hardpoints reference weapon IDs and declare which weapons are allowed per slot:
```toml
[[hardpoints]]
slot    = 0
type    = "missile"
allowed = ["aim120c", "aim7m", "aim9x"]
default = "aim120c"

[[hardpoints]]
slot    = 4
type    = "bomb"
allowed = ["gbu12", "mk82", "agm65"]
default = "mk82"
```

### Ground & Naval Unit Data — TOML
Ground and naval units share a format; the dynamic campaign unit pools reference these IDs.
The FA bridge translates FA's OT/NT entity types to equivalent unit TOML structs.

```toml
# units/sa10_battery.toml — Surface-to-air missile battery
[unit]
id     = "sa10_battery"
name   = "SA-10 Grumble Battery"
type   = "sam"
mesh   = "sa10"
mobile = false

[armor]
rating = 2          # 0=soft, 1=light, 2=medium, 3=heavy; affects weapon damage multiplier
health = 100

[radar]
emitter_id   = "sa10_search"   # activates this RWR emitter when searching
track_range_nm = 90
can_shutdown   = true          # will shut down radar to evade HARM

[[weapons]]
weapon_id    = "s300_missile"
max_range_nm = 90
max_alt_ft   = 100000
targets      = ["air"]

[ai]
script = "ai/units/sam_battery.lua"
```

```toml
# units/t80_platoon.toml — Armored vehicle group
[unit]
id     = "t80_platoon"
name   = "T-80 Platoon"
type   = "armor"
mesh   = "t80"
mobile = true

[armor]
rating = 3
health = 100

[movement]
max_speed_kph = 65
prefers_roads = true

[[weapons]]
weapon_id    = "125mm_gun"
max_range_ft = 8000
targets      = ["ground"]

[ai]
script = "ai/units/armor_advance.lua"
```

```toml
# units/burke_ddg.toml — Surface combatant
[unit]
id     = "burke_ddg"
name   = "Arleigh Burke DDG"
type   = "destroyer"
mesh   = "ddg51"
mobile = true

[movement]
max_speed_kts = 30
type          = "surface"

[[weapons]]
weapon_id    = "sm2_missile"
max_range_nm = 90
targets      = ["air"]

[[weapons]]
weapon_id    = "phalanx_ciws"
max_range_nm = 1
targets      = ["air", "missile"]   # last-ditch defense

[ai]
script = "ai/units/surface_ship.lua"
```

### Mission Files — YAML
```yaml
name: "Storm Warning"
map: ukraine
layer: ukraine_clear
time: { hour: 14, minute: 0 }
wind: { heading: 270, speed: 12 }
sides: [nato, russia]

objects:
  - type: F22
    id: player1
    side: nato
    pos: [12400, 0, 8800]
    heading: 90
    alt: 500

  - type: SA10
    id: sam1
    side: russia
    pos: [15000, 0, 9000]

triggers:
  - on: destroy(sam1)
    do: mission_success
```

### Terrain — Streaming Heightmap Chunks + JSON
- Grid of fixed-size PNG chunks; any grid dimension (no 32×32 cap)
- Each chunk: 513×513 pixels, 16-bit grayscale (height precision ~0.15 m/step at 1000 m range)
- Chunks loaded/unloaded at runtime based on player position; no single-heightmap load
- Surface class overlay: separate chunk grid at lower resolution (terrain type per texel)
```json
{
  "name": "Ukraine",
  "chunk_size_ft": 6076,
  "grid_width": 64,
  "grid_height": 64,
  "elevation_scale": 10,
  "chunks_dir": "terrain/ukraine/",
  "surface_classes_dir": "terrain/ukraine_surface/",
  "textures": {
    "0": "terrain_grass.ktx2",
    "1": "terrain_water.ktx2",
    "2": "terrain_urban.ktx2"
  }
}
```

### Campaign Files — YAML
- Hybrid authored + dynamic campaign model
- **Authored story missions**: hand-crafted YAML missions at key narrative beats; designer
  controls exact objectives, dialogue, and scripted events
- **Dynamic layer**: a lightweight ground-war simulation runs between story missions;
  generates procedural sorties (intercept, CAP, strike, SEAD) from frontline state;
  player outcomes shift territory and unit counts
- Story missions inject at dynamic trigger points and lock the simulation temporarily;
  outcomes may reset or advance the frontline before dynamic resumes
```yaml
name: "Forgotten Skies"
version: "1.0"
sides: [nato, russia]
pilot:
  rank_table: ranks/nato_ranks.toml
  persistent_stats: true

# Dynamic ground-war layer
dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/ukraine_start.png   # white=nato, black=russia
      ground_units:
        nato:   { armor: 40, infantry: 60, artillery: 20 }
        russia: { armor: 55, infantry: 80, artillery: 30 }
      # Mission templates generated from frontline state; engine fills positions/counts
      templates:
        - { type: intercept,    file: templates/ukraine_intercept.yaml }
        - { type: cap,          file: templates/ukraine_cap.yaml }
        - { type: strike,       file: templates/ukraine_strike.yaml }
        - { type: sead,         file: templates/ukraine_sead.yaml }

# Authored story beats override the dynamic layer at narrative checkpoints
story:
  - id: u01_storm_warning
    file: missions/u01.yaml
    label: "Storm Warning"
    trigger: campaign_start
    locks_dynamic: true
    on_complete:
      set_frontline: frontlines/ukraine_after_u01.png
      unlock: ukraine
      next: { after_sorties: 3, id: u02_iron_fist }

  - id: u02_iron_fist
    file: missions/u02.yaml
    label: "Iron Fist"
    trigger: { theater: ukraine, sorties_since_last_story: 3 }
    locks_dynamic: true
    on_complete:
      next:
        - { if: "ukraine.nato_territory > 0.6", id: u03_endgame_victory }
        - { else: u03_endgame_retreat }
```

### Faction Data — TOML
Factions are standalone TOML files in `factions/`. Campaigns and missions reference
faction IDs. Mods add new factions by including additional TOML files — no engine changes
required. The FA bridge maps FA's hardcoded sides to equivalent faction entries.

```toml
# factions/nato.toml
[faction]
id           = "nato"
name         = "NATO"
short        = "NATO"
color        = "#4488FF"          # HUD label color for this faction
icon         = "icons/nato.png"   # map/browser icon

# Default diplomatic relationship with other factions (overridable per campaign/mission)
[relationships]
russia       = "hostile"
china        = "neutral"          # neutral = no auto-engage; mission triggers can change
un           = "friendly"
```

```toml
# factions/russia.toml
[faction]
id    = "russia"
name  = "Russia"
short = "RUS"
color = "#FF4444"
icon  = "icons/russia.png"

[relationships]
nato  = "hostile"
china = "friendly"
un    = "neutral"
```

Relationship values: `friendly` (no engagement, share radar tracks), `neutral` (no
auto-engage, no track sharing), `hostile` (AI auto-engages on detection). Missions and
Lua scripts can override any relationship at runtime (`world.set_relationship(a, b, state)`).

### HUD Layout — TOML
HUD element layout is a native engine format. Content packs and mods can ship alternate
layouts; the player can override any element via the in-game drag-and-drop editor.
The FA bridge translates FA's binary `.HUD` anchor data into this format on first load.

```toml
# config/hud_layout.toml  (also shippable as part of a content pack at hud/<name>.toml)
[layout]
name    = "Standard"
preset  = true      # appears in the Presets dropdown

[[elements]]
id       = "radar_scope"
pos      = [0.05, 0.60]   # normalized screen coords (0,0 = top-left)
scale    = 1.0
opacity  = 0.90
visible  = true

[[elements]]
id       = "rwr_strip"
pos      = [0.88, 0.50]
scale    = 1.0
opacity  = 0.85
visible  = true

[[elements]]
id       = "airspeed"
pos      = [0.10, 0.50]
scale    = 1.0
opacity  = 1.0
visible  = true

# Additional elements: altitude, g_meter, fuel, weapon_select,
# minimap, objective_list, wingman_sidebar, threat_labels
```

Font rendering uses a bitmap glyph atlas: a PNG sprite sheet paired with a JSON metrics
file (`fonts/<name>.png` + `fonts/<name>.json`) that maps each character to its rect,
advance width, and baseline offset. Content packs provide font atlases; the engine ships
one default font. The FA bridge converts FA's FNT bitmap font format to this layout.

### Rank Table — TOML
Rank progression is data-driven. Campaigns reference a rank table; the engine reads it
to determine when a pilot earns a promotion and what title to display. No rank logic is
hardcoded in the engine.

```toml
# ranks/nato_ranks.toml
[[ranks]]
id          = "trainee"
title       = "Trainee"
min_score   = 0
icon        = "icons/ranks/trainee.png"

[[ranks]]
id          = "lieutenant"
title       = "Second Lieutenant"
min_score   = 500
icon        = "icons/ranks/lt.png"

[[ranks]]
id          = "captain"
title       = "Captain"
min_score   = 2000
icon        = "icons/ranks/cpt.png"

[[ranks]]
id          = "major"
title       = "Major"
min_score   = 5000
icon        = "icons/ranks/maj.png"

[[ranks]]
id          = "colonel"
title       = "Colonel"
min_score   = 12000
icon        = "icons/ranks/col.png"

[[ranks]]
id          = "ace"
title       = "Ace"
min_score   = 25000
icon        = "icons/ranks/ace.png"
```

`min_score` is in campaign score points; campaigns define how many points each objective
and kill type awards. Mods add custom rank tables and reference them in campaign YAML.
The FA bridge translates FA's fixed PLT rank tiers to an equivalent rank table on import.

### AI Scripts — Lua 5.4
- Each AI entity has one or more Lua coroutines; behaviors can be stacked and interrupted
- Engine exposes a sandboxed `ai` API: navigation, sensors, weapons, comms, world queries
- FA's goto-script LABEL/CONDITION/ACTION model translates to a Lua state-machine wrapper
- Scripts live in `ai/<role>.lua`; multiple entities can share the same script
```lua
-- ai/interceptor.lua
local function patrol(self)
  ai.fly_waypoints(self, self.waypoints)
  while true do
    local threat = ai.scan_threats(self, range_nm(40))
    if threat then
      ai.radio(self, "BANDIT", threat)
      return engage(self, threat)
    end
    coroutine.yield()
  end
end

local function engage(self, target)
  ai.set_afterburner(self, true)
  while ai.in_range(self, target, range_nm(10)) == false do
    ai.fly_to(self, ai.position(target))
    coroutine.yield()
  end
  ai.fire_missile(self, target, "aim120c")
  return patrol(self)
end

return { init = patrol }
```

## Phase 0 — Complete the Toolkit
**Duration: 2–4 weeks | Repo: fighters-toolkit**
**Milestone: fighters-toolkit declared feature-complete for FA modders**

### 0.1 PLT Stats Block (the one open TODO.md item)
- Differential save pass: vary rank / score / missions / weapons in-game,
  compare binary PLT files at the four gap ranges:
  - `0xB0–0xC1` (18 bytes — score level / rank index)
  - `0xCF–0x5AE` (1,344 bytes — between secondary string and mission log)
  - `0x2018–0x20B7` (160 bytes — between kill tallies and weapon accuracy)
  - `0x21F8–0x25DF` (~1,000 bytes — fort/campaign-phase stats and MP scoring)
- Update `docs/fa/formats/PLT.md`.
- Implement `plt_stats` parse/serialize in `lib/src/plt.cpp`.
- Expand ft-gui PLT editor to show full stats block.
- Add `ft plt dump` CLI subcommand.

### 0.2 SH 3D Model Viewer in ft-gui
- Add 3D preview panel rendering `sh_parse_mesh()` output via DX11.
- Static geometry pass only until SH animation opcodes are reversed (Phase 1B).

### 0.3 Test Coverage Expansion
- Catch2 round-trip tests for all major codecs: `pic`, `audio`, `seq`,
  `brf`/`ot`/`pt`/`jt`, `mission`, `sh`, `t2`, `plt`.

### 0.4 Minor RE Items (feed Phase 1 engine work)
- **PT_TYPE byte offsets**: trace `SetupPT` at `0x4A7220`; update `PT.md`. (~3–5 days)
- **T2 tile-summary algorithm**: single Ghidra trace; update `T2.md`. (~1–2 days)

## Phase 1 — Engine Foundation + FA Bridge + RE Gap Closure
**Duration: 10–14 weeks | Three parallel workstreams**
**Milestone: Engine compiles on all platforms; FA content plugin runs; renderer RE gaps closed**

### Workstream A — Engine Core Setup

**A.1 Repo and CMake skeleton**
- `CMakeLists.txt` root: C++20, cross-platform.
- Subdirs: `engine/` (core), `platform/` (HAL), `plugins/` (content packs),
  `tools/` (dev utilities), `tests/`.
- Submodule: `external/fighters-toolkit`; link `ft_lib` (used only inside `plugins/fa-content/`).
- FetchContent / vcpkg: Vulkan SDK + MoltenVK, SDL3, OpenAL Soft, Catch2.
- `plugins/fa-content/` additionally depends on FluidSynth + a GM soundfont for XMI
  rendering; these are build dependencies of the FA bridge only, not the engine core.

**A.2 Platform HAL interfaces**
```
platform/IWindow.h      — create/destroy window, event pump, resize
platform/IRenderer.h    — swap chain, command buffer submission
platform/IAudio.h       — buffer upload, source play/stop/position
platform/IInput.h       — keyboard, mouse, gamepad (SDL3 GameController API)
platform/INetwork.h     — UDP/TCP socket send/recv
platform/IFilesystem.h  — directory scan, file open (no LIB-specific code here)
```

**A.3 Content Pack and Mod System** ← new; defined here alongside HAL
```
engine/content/IContentPack.h   — interface (see above)
engine/content/ModLoader.cpp    — scans mods/, loads manifests, builds stack
engine/content/AssetManager.cpp — resolves asset requests through content stack
                                    case-insensitive lookup; ref-counted handles
```
The `AssetManager` is the only point in the engine that calls `IContentPack` methods.
No other engine code knows what format any asset came from.
- **Asset hot-reload** (sandbox and editor mode only): `AssetManager` watches mod
  directories for file changes via platform filesystem notifications. On change:
  Lua scripts are reloaded and live coroutines restarted with preserved entity state;
  TOML flight models and weapon data are re-parsed and applied to existing entities;
  YAML missions reload trigger state. Textures and meshes require a scene reload.
  Hot-reload is disabled in campaign mode to prevent save-state inconsistency.
  This is the primary mod development workflow — edit, save, see result immediately.

**A.4 Vulkan + MoltenVK renderer backend**
- `platform/vulkan/VkWindow.cpp` — SDL3 + `SDL_Vulkan_CreateSurface`.
- Swap chain, render pass, command pool, descriptor layout skeleton.
- Debug validation layers in dev builds.
- MoltenVK path verified on macOS arm64.

**A.5 SDL3 windowing + input backend**
- `platform/sdl3/SDL3Window.cpp`, `SDL3Input.cpp`.
- Gamepad axis mappings (throttle, stick) via SDL3 GameController API.
- `engine/input/InputBindings.cpp` — persistent key/button/axis binding table; saved to
  `config/bindings.toml`.
- `engine/input/AxisConfig.cpp` — per-axis dead zone, response curve (linear/cubic/custom),
  invert flag. Supports HOTAS throttle axes mapped to separate controls (throttle, rudder,
  brake, trim).
- In-game binding configurator UI: list all actions, click to rebind, detect conflicts.
- **Controller haptics**: SDL3 rumble API used for immersive feedback events — G-force
  onset (sustained rumble scaling with current G-load), weapon fire (short pulse per
  shot), missile launch (sharp spike), landing gear touchdown (thud), arrested wire catch
  (hard jolt), and engine damage (asymmetric low-frequency rumble). Intensity is a
  0–100% slider in Settings → Controls; set to 0 to disable. Haptics only fire when
  the controller reports rumble support; no effect on HOTAS hardware that lacks motors.

**A.6 OpenAL Soft audio backend**
- `platform/openal/OALAudio.cpp` — device open, context, 3D listener.
- PCM buffer upload; OGG streaming for music (via stb_vorbis or similar; no MIDI dependency).
- Listener position updated from player entity world coords each frame.

**A.7 ENet networking backend**
- `platform/net/ENetTransport.cpp` — ENet host/peer management; reliable + unreliable channels.
- `fl-server` build target: headless server binary sharing the same `engine/` core, no rendering.
- `WSAStartup` shim on Windows; no-op on POSIX.
- BSD raw sockets retained only for fa-content bridge FA-protocol compatibility path.

**A.9 First-Run Experience**
The critical first impression for a player who has no mods installed.

- Engine detects no content packs on startup and shows a **Welcome screen** instead of
  the main menu.
- Welcome screen presents three paths, clearly labelled:
  1. **"I own Fighters Anthology"** — walks the player through locating their FA
     installation (auto-detects from registry / `FA_INSTALL_DIR`; shows a folder browser
     if not found); downloads and installs the fa-content bridge from the community index;
     launches into training on completion.
  2. **"I don't own the game"** — opens the mod browser filtered to free base packs;
     player installs a community content pack; launches into training on completion.
  3. **"I'm a mod developer"** — skips content install; opens directly into sandbox mode
     with an empty map and the in-game mission editor active; shows a link to modding docs.
- After first-run completes, the Welcome screen never appears again (flag in
  `config/user.toml`). Players can re-run it from Settings → Content.
- If FA is detected automatically (registry key present), path 1 is pre-selected and the
  folder step is skipped; one-click install proceeds immediately.

**A.8 Localization infrastructure**
Ship i18n support from day one so community translations can land without engine changes.

- All user-visible strings in engine and UI are keyed (`ui.main_menu.campaign`,
  `hud.rwr.lock_warning`, etc.); never hardcoded in source.
- String tables stored as TOML files: `locale/<lang>/engine.toml`,
  `locale/<lang>/ui.toml`. Default locale: `en`.
- Locale selection in Settings → Language; falls back to `en` for any missing key.
- **Mod localization**: mod manifests can include a `locale/` directory; mod-added UI
  strings (custom HUD elements, mission briefing text) follow the same key scheme.
- **Community translations**: accepted as pull requests to the main repo or packaged as
  standalone locale mods installable from the mod browser. No official translations are
  maintained by the project — all community-contributed.
- Subtitle strings (Phase 2.6) use the same key scheme; translating subtitles is the
  same workflow as translating UI text.

**A.8 CI/CD**
- GitHub Actions matrix: `windows-latest` (MSVC), `ubuntu-latest` (GCC/Clang),
  `macos-latest` (AppleClang, arm64).
- Build-only initially; tests added as engine code accumulates.

**A.11 Graphics / video settings**
Saved to `config/user.toml`. All settings take effect without restart unless noted.

| Setting | Options | Default | Notes |
|---|---|---|---|
| Resolution | Any desktop resolution | Native | Windowed/borderless/fullscreen toggle |
| V-sync | Off / On / Adaptive | On | Adaptive = EXT_swap_control_tear if supported |
| Frame rate cap | Off / 30 / 60 / 120 / 144 / 240 | Off | Applied on top of V-sync |
| Quality preset | Low / Medium / High / Ultra | High | Sets shadow, particle, and draw-distance sliders |
| Shadow quality | Off / Low / Medium / High | Medium | Cascade count + resolution |
| Particle density | Low / Medium / High | Medium | GPU particle budget multiplier |
| Draw distance | Low / Medium / High / Ultra | High | Terrain chunk prefetch radius |
| Anti-aliasing | Off / FXAA / MSAA 2× / MSAA 4× | FXAA | MSAA requires restart |
| UI scale | 75% / 100% / 125% / 150% | 100% | Scales all HUD and menu elements; aids high-DPI displays |
| Cockpit FOV | 60°–120° | 90° | Saved per pilot profile |

Low preset targets integrated graphics (Intel Iris, Apple M-series GPU). Medium targets
a discrete GPU from 4–6 years ago. The quality preset sets all sliders at once; each
can then be individually overridden. The engine emits a Vulkan validation warning (not
an error) if the requested settings exceed GPU capability and falls back gracefully.

**Audio mix settings** (also `config/user.toml`):

| Slider | Range | Default |
|---|---|---|
| Master volume | 0–100% | 80% |
| Sound effects | 0–100% | 100% |
| Music | 0–100% | 70% |
| Voice chat | 0–100% | 100% |
| RWR / cockpit alerts | 0–100% | 100% |

RWR alerts have a separate slider because they must remain audible even when SFX are
turned down for stream/recording setups. All sliders save immediately on change.

**A.10 Crash reporting and error feedback**
- On unhandled exception or signal: write a crash dump to `logs/crash_<timestamp>.log`
  containing engine version, OS, GPU driver version, active mod list with versions, and
  the last 200 log lines before the crash.
- On startup after a crash: show a dialog — "Fighters Legacy crashed last session. Would
  you like to view the crash log or report it?" — with buttons: View log (opens file
  manager), Report (opens a pre-filled GitHub issue with the log attached), Dismiss.
- No automatic telemetry or data transmission without explicit user action; all crash
  data stays local until the player chooses to report.
- In-engine log (`logs/engine_<date>.log`): rolling log, one per session, capped at 10
  retained files. Log level configurable via `--log-level` CLI flag (error / warn /
  info / debug). Debug level logs every asset load, entity spawn, and Lua call — useful
  for mod developers diagnosing issues.
- Mod load failures (missing dep, version conflict, bad manifest) produce a clear error
  dialog at startup listing each failed mod and the reason; the engine continues with
  the remaining valid mods rather than refusing to start.

### Workstream B — FA Content Bridge Plugin

**B.1 Plugin skeleton**
- `plugins/fa-content/CMakeLists.txt`: optional target, `-DFL_FA_CONTENT=ON`.
- Implements `IContentPack`; links `ft_lib`.
- Manifest: `plugins/fa-content/manifest.toml` with `requires-fa = true`.

**B.2 FA install discovery**
- Read `FA_INSTALL_DIR` env var or registry key; prompt dialog via SDL3 if absent.
- Mount all FA LIB archives via `ft_lib ealib`; store as internal state.

**B.3 Asset translation layer**
Implement each `IContentPack` method. Priority order:

| Method | Translation | Effort |
|---|---|---|
| `loadMesh` | `sh_parse_mesh()` → `MeshData` | Medium (SH static pass already works for 94.9%) |
| `loadTexture` | `pic_decode()` + `pal` → RGBA `TextureData` | Low (ft_lib already does this) |
| `loadFlightModel` | `brf_parse(*.PT)` → `FlightModel` | Low (fields documented; needs PT offsets from 0.4) |
| `loadAIScript` | `ai_parse(.AI)` → translate goto-script → Lua compat wrapper | Low |
| `loadMission` | `mission_parse(*.M)` → `MissionData` | Low (ft_lib already parses .M) |
| `loadTerrain` | `t2_parse()` → `TerrainData` | Medium |
| `loadAudio` (SFX) | `ft_lib audio_decode()` raw PCM → OGG `AudioBuffer` | Low |
| `loadAudio` (music) | `ft_lib mus_decode()` XMI → MIDI → FluidSynth PCM → OGG; cached to `cache/fa-content/music/` | Medium (FluidSynth render is slow; caching is essential) |
| `listAssets` | enumerate mounted LIBs | Low |

**B.4 Translation cache**
- Optional on-disk cache at `cache/fa-content/<asset>.bin`.
- Invalidated if FA LIB mtime changes.
- Makes repeat launches fast without re-parsing LIB archives.

### Workstream C — RE Gap Closure

Output goes to `docs/fa/` in fighters-toolkit. Run parallel to A and B.

**C.1 Audit OpenFA (GitLab, Rust) — 1 week**
Audit their source for already-reversed SH opcodes before starting independent RE.
Document confirmed semantics in `docs/fa/formats/SH.md`.

**C.2 SH animation / LOD / damage opcodes — 4–6 weeks (critical path)**
Handlers in FA.EXE `0x4D0000–0x4EFFFF`. Priority:

| Opcode | Name | Why it matters |
|---|---|---|
| `0xAC 0x00` | `JumpToDamage` | Damage-state geometry |
| `0xC8 0x00` | `JumpToLOD` | LOD switching |
| `0x40 0x00` | `JumpToFrame` | Animation (gear, props, bay doors) |
| `0xC4/C6 0x00` | `XformUnmask` / `XformUnmaskLong` | Conditional transform-gated render |
| `0x12/6E 0x00` | `Unmask` / `UnmaskLong` | Entity state visibility gate |
| `0x42 0x00` | `SourceName` | Secondary asset reference |
| `0x06–0x10 0x00` | `Unk06/08/0C/0E/10` | Material / lighting / effect flags |
| All remaining `Unk*` | — | Sizes known; confirm semantics |

**C.3 Sky/horizon renderer internals — 1 week**
Decompile `_SolidHorizon`, `_GouraudHorizon`, `@G_Tile@32`, `_GRExec_4`.
Document `_GRExec_4` short-int command stream format. Update `RENDERER.md`.

**C.4 `vector_table` render dispatch — 1–2 weeks**
Map 141-xref dispatch table at `0x5183A0`. Update `RENDERER.md`.

**C.5 GRAPHIC effect spawn parameters — 1 week**
Full parameter layouts for `_GRAPHICAddExp`, `_GRAPHICAddSmoke`, `_GRAPHICAddDebris`,
`_GRAPHICAddFire`, `_GRAPHICAddSmokeAdder`, `_GRAPHICAddHulk`, `_GRAPHICAddCrater`,
`_GRAPHICAddDevice`, and `_explode` entity layout at `entity+0x2234`. Update `RENDERER.md`.

## Phase 2 — Modern-Particles Engine
**Duration: 14–18 weeks (starts week 10, after HAL + FA bridge are stable)**
**Milestone: FA missions playable on Windows, Linux, macOS with GPU effects**

All asset access goes through `AssetManager` → `IContentPack` (FA bridge initially).

### 2.1 Game Loop (weeks 10–11)
- Fixed-timestep loop; separate sim thread + shell thread.
- Per-tick time update; time compression (½×, 1×, 2×, 4×, 8×, paused).
- Frame gating: sim tick skipped when no time has elapsed.

### 2.2 Entity / Object System (weeks 10–12)
- Dynamic object pool; no hard caps (soft limits configurable per server via TOML).
  FA's 300,000-byte pool and 799/899 limits are lifted; the fa-content bridge enforces
  them only in classic/parity mode if exact parity is required.
- Component dispatch by object type: ground vehicle, air vehicle, projectile, player,
  effect. Types registered by content packs; engine has no hardcoded type list.
- Entity death: death flag, score event, scenario evaluation — driven by mission triggers.
- **Progressive damage system**: aircraft damage is tracked on multiple thresholds defined
  per aircraft TOML (light / heavy / critical). Each threshold triggers visual effects
  (smoke, fire, sparks via particle system) and gameplay penalties (avionics failure,
  engine thrust reduction, control surface sluggishness). Death is not binary: a critically
  damaged aircraft can limp home. Damage state selects the correct SH geometry variant
  (JumpToDamage) in classic mode; in modern mode, damage overlays are shader-driven.

### 2.3 Flight Model (weeks 12–15)
- `FlightModel` struct populated via `AssetManager.loadFlightModel()` (FA bridge
  translates BRF; future packs provide TOML directly). The FA bridge requires Phase 0.4
  PT_TYPE byte offsets to populate the struct correctly; the engine FlightModel struct
  is defined independently of FA's PT format.
- Flight update: stall state machine, fuel burn, G-drag, thrust (mil + AB),
  wing sweep, TVC nozzle, flight envelope (altitude-band table).
- Collision: swept-sphere vs. terrain + mesh + AABB.
- Terrain height query from elevation bands.
- Terrain avoidance: 250 ft lookahead pitch correction.
- **Flight assists** (per-player toggles saved in pilot profile):
  - *Auto-leveling*: pitch/roll damping; useful for keyboard/gamepad players.
  - *G-limiter*: prevents departure stalls; engine refuses inputs that would cause departure.
  - *Auto-throttle*: maintains a target airspeed.
  - *Simplified landing*: widens the acceptable touchdown envelope.
  - All assists off = closest to original FA feel; all on = fully arcade.
- **Aim assist** (separate toggle): missile reticle snaps toward nearest valid target within
  seeker cone; gun lead indicator always shown regardless of radar lock.
- **In-flight refueling** (aircraft flagged `refuel_capable: true` in TOML):
  Three difficulty tiers controlled by the accessibility settings:
  - *Auto-refuel* (Cadet): fly within 1 nm of a tanker at similar speed, press the refuel
    key; fuel transfers at full rate instantly; no formation skill required.
  - *Simplified* (Pilot): fly into a generous contact cone behind the tanker; game snaps
    the boom/drogue connection; player must hold heading and throttle to maintain position
    but small deviations are forgiven; disconnect if you drift too far outside the cone.
  - *Manual* (Ace): precise position required in a small contact envelope; turbulence
    perturbs position; boom operator callouts ("move forward / back / left / right");
    hard disconnect on excessive deviation; fuel transfer rate tied to connection quality.
  - Tanker is an AI entity following a racetrack waypoint orbit defined in mission YAML;
    player requests contact via radio menu (wingman command interface); tanker acknowledges
    and holds course.
  - Aircraft TOML fields:
    ```toml
    [refueling]
    capable          = true
    type             = "boom"      # or "drogue"
    max_rate_lb_min  = 600
    ```
  - Mission YAML tanker entry:
    ```yaml
    - type: KC135
      id: texaco1
      side: nato
      role: tanker
      waypoints: [refuel_wp1, refuel_wp2]   # racetrack orbit
      fuel_available_lb: 100000
      offload_rate_lb_min: 600
    ```
  - Training module: dedicated refueling practice mission available in training mode.
- **Carrier operations** (aircraft flagged `carrier_capable: true` in TOML):
  - *Catapult launch*: player holds brake + full throttle; catapult applies impulse force
    over ~2 seconds to reach flying speed; abort possible until cat fires.
  - *Arrested landing*: four wire zones on the deck; wire catch requires correct approach
    speed (on-speed AoA) and glideslope; bolter (missed all wires) requires immediate
    go-around; crash landing applies damage.
  - *FLOLS/meatball*: HUD glideslope indicator (ball + datum lights) visible on approach;
    suppressed at > 3 nm; replaces ILS approach in carrier ops.
  - Carrier entity moves at set heading and speed; all deck positions track ship frame.

### 2.4 AI System (weeks 13–15)
- Lua 5.4 embedded runtime (LuaJIT optional for performance).
- Sandboxed `ai` API module: navigation, sensor queries, weapon control, comms, world state.
- Coroutine-based behavior scheduler: each entity runs one or more Lua coroutines; engine
  resumes them each sim tick; behaviors can yield, stack, and interrupt cleanly.
- FA compatibility: all FA AI command and evaluation semantics reimplemented as `ai` API calls;
  FA goto-scripts translated to Lua state-machine wrappers by the fa-content bridge.
- NPC waypoint navigation driven from native YAML mission waypoints; FA `.WP` data
  translated to the same struct by the bridge.
- **Wingman command interface**: F-key radio menu (FA-style) and optional keybinds for
  common commands. Commands are Lua calls into the targeted wingman's behavior:
  - *Attack my target* — wingman engages current player lock
  - *Cover me* — wingman protects player, engages threats to player first
  - *Return to base* — wingman disengages and navigates to friendly airfield
  - *Rejoin formation* — wingman returns to player's wing position
  - *Go to waypoint N* — wingman navigates independently to a mission waypoint
  - *Break off* — wingman disengages current engagement
  - Wingmen acknowledge with callout audio; status visible on HUD sidebar.
- Wingman commands are data-driven: mission YAML assigns callsigns and command availability
  per slot; mods can add custom commands via the Lua AI API.
- **Formation slot positions**: explicit geometric slots defined relative to the player.
  Standard formations: Echelon Left, Echelon Right, Line Abreast, Trail, Vic (3-ship).
  Player cycles formation type via a key binding; each wingman is assigned a numbered slot.
  "Rejoin formation" commands the wingman to fly to their assigned slot and hold it using
  a Lua coroutine that continuously corrects heading, altitude, and speed to maintain
  offset. Slot offsets (distance, bearing, altitude delta) are configurable per formation
  type in `data/formations.toml`; mods can add custom formation types.
- **AWACS entity**: an aircraft with `role: awacs` in mission YAML runs a special AI
  script that continuously scans for all contacts within extended range and shares tracks
  to all friendly players via datalink — contacts appear on player radar scopes even
  beyond their own radar range. AWACS callouts ("Bogey, bearing 270, 40 miles, angels 20")
  are audio events triggered by the AI script. AWACS is a high-value target; its loss
  degrades all friendly radar awareness. Players in the cooperative AWACS coordinator slot
  (2.7) use the same datalink feed. FA bridge maps FA's AWACS support calls to this system.

### 2.5 Mission / Campaign Loader (weeks 14–17)
- `MissionData` populated via `AssetManager.loadMission()` (FA bridge translates `.M`/`.MM`;
  native content provides YAML directly).
- `CampaignData` populated via `AssetManager.loadCampaign()` (FA bridge translates `.CAM`;
  native content provides YAML graph directly).
- Native campaign system: arbitrary theater graph, any number of theaters, branching on
  mission outcomes or pilot stats, persistent world state between missions. No 6-theater cap.
- Faction loader: reads all `factions/*.toml` from active content packs; builds
  relationship matrix used by AI auto-engage logic, HUD color coding, and dynamic
  campaign side assignment. Missions and Lua scripts can override relationships at runtime.
- Condition evaluator: C++ with Lua extensibility; FA's `.MC` binary conditions mapped
  to built-in condition functions; native YAML missions use plain Lua expressions.
- **No content gating in sandbox**: all aircraft, maps, weapons, and game modes are
  available in sandbox and instant action regardless of campaign progress. The campaign
  layer voluntarily restricts access to create a progression experience; the engine
  itself never locks anything. Campaign unlocks are advisory metadata in the campaign
  YAML, not enforced by the engine.
- **Mission scripting Lua API**: event hooks accessible in any mission YAML or standalone
  Lua script. Covers everything the visual trigger editor generates, plus direct world
  manipulation for advanced creators:
  ```lua
  -- Hooks: any engine event drives any world action
  on("entity_killed", "sam1", function()
    world.add_objective("Safe corridor open — proceed north")
    world.spawn_entity({ type="F16", side="nato", pos={14000,0,9500}, ai="escort" })
  end)

  on("time_elapsed", 600, function()
    world.set_weather("storm")
    world.radio_message("all", "Weather deteriorating — egress recommended")
  end)

  on("player_entered_zone", "danger_zone", function(player)
    world.set_threat(player, "active")
  end)
  ```
  The Lua mission API is the same API used internally by the dynamic campaign generator
  and the visual trigger editor — no privileged surface.
- **Dynamic campaign subsystem** (`engine/campaign/DynamicCampaign.cpp`):
  - Ground-war simulation: unit pools per side per theater; frontline polygon advances or
    retreats after each sortie based on objectives completed and units destroyed.
  - Mission demand generator: polls frontline state after each sortie; selects a template
    that fits current tactical situation (enemy aircraft active → intercept, SAM coverage
    high → SEAD, etc.); instantiates positions and unit counts from frontline data.
  - Story mission injector: checks authored trigger conditions each time a new sortie is
    offered; story missions pre-empt dynamic when triggered; frontline may be reset by
    story outcome before dynamic resumes.
  - **Persistent target destruction**: destroyed named entities (SAM sites, airfields,
    bridges, radar stations) are recorded by ID in the campaign save. On subsequent
    sorties over the same theater, these entities are not spawned. Their destruction is
    reflected in the frontline state (no SAM coverage in that sector → adjacent territory
    easier to contest). Unnamed units (generic tank platoons, interceptor flights) are
    regenerated from pool counts, not tracked individually. This makes player actions feel
    durable without requiring infinite entity tracking.
  - All state serialized to pilot TOML save (frontline PNG + unit counts + destroyed-entity
    ID list + sortie log).
- Three-phase mission init: content loading → entity spawn → trigger registration.
- Pilot save: full round-trip via `AssetManager`; native format is TOML (FA PLT translated
  on import; no PLT binary format exposed to the engine).
- **Pre-mission loadout screen**: shows aircraft 3D model with hardpoint slots; player
  selects weapon per slot from mission-allowed list; fuel load slider; loadout affects
  aircraft weight/drag in the flight model. FA bridge pre-populates loadouts from `.M` data.
- **After-action debrief screen**: shot accuracy per weapon type, time on target, friendly
  fire events, objectives completed, score breakdown, kills by category (air/ground/naval),
  countermeasures used. Saved to pilot TOML profile.
- **Ground rearm and refuel**: configurable per context — not hardcoded to any one mode.
  - `instantaneous` (sandbox default, FA behavior): land at a friendly airfield, press
    rearm key; weapons and fuel restored immediately. Keeps momentum.
  - `timed`: rearm takes 60–120 seconds of sim time (time-compressible); tension when a
    second wave is inbound. No resource cost, just time.
  - `supply-limited`: each airfield has a weapon supply pool drawn from at each rearm;
    pool restocks over time or via supply convoy missions generated by the dynamic campaign.
    Adds a logistics layer — bombing enemy supply depots degrades their rearm capability.
  Sandbox mode defaults to `instantaneous` but the session config screen lets the player
  choose. Campaign YAML sets `rearm_mode` per theater. Difficulty table adds a rearm row.
  Server operators set mode in `fl-server` config; cannot be overridden by clients.
- **Training mode**: dedicated campaign section available from the main menu; does not
  affect pilot campaign save. Covers: takeoff, basic maneuvers, weapons employment,
  landing, formation flying, combat maneuvering. Each module is a YAML mission with
  guided triggers and on-screen prompts. Community-contributed training packs are
  supported as content pack missions tagged `category: training`.
- **Ejection and pilot loss**:
  - Ejection over friendly territory: pilot recovered; aircraft lost; campaign continues
    with a replacement airframe (counts against sortie resources in campaign).
  - Ejection over hostile territory: pilot MIA; stronger campaign score penalty; affects
    rank progression. Recovery possible if a CSAR mission is generated by the dynamic layer.
  - Catastrophic kill (no ejection): same outcome as hostile-territory ejection.
  - In sandbox and instant action: no pilot consequence by default; optional
    "pilot permadeath" setting ends the session on pilot loss.
  - FA bridge maps FA's `EJECT.SH` trigger to the engine's eject event.
- **Multiple pilot profiles**: `pilots/` directory; each pilot is a named TOML file.
  Profile select screen on first launch and in Settings. Each profile stores: callsign,
  campaign saves, lifetime stats, difficulty preset, and optionally a personal binding
  override (inherits global bindings by default). Switching profiles reloads the content
  stack with that pilot's active mod list.

### 2.6 Audio (weeks 15–17)
- Positional audio via OpenAL Soft; listener updated from player entity each frame.
- OGG playback via `AssetManager.loadAudio()`; engine never sees raw PCM, XMI, or MIDI.
  FA bridge translates FA SFX (raw PCM → OGG) and FA music (XMI → FluidSynth → OGG,
  cached to disk) before handing `AudioBuffer` to the engine. Native content provides
  OGG directly.
- All 9 FA MUS playlists translated at first launch and cached; game-state transitions
  drive playlist changes identically for FA and native content.
- **Subtitles for audio callouts**: all in-game voice events (RWR warnings, LSO approach
  calls, wingman acknowledgments, AWACS contacts, radio messages) have an associated
  subtitle string in their audio event definition. A subtitle overlay on the HUD shows
  the text for a configurable duration when the audio plays. Toggle in Settings →
  Accessibility. Content packs provide subtitle strings alongside audio assets; the
  FA bridge maps FA's hardcoded callout text to subtitle strings. Subtitle text is
  localisation-ready (keyed strings, not hardcoded).

### 2.7 Networking (weeks 16–19)

**Native multiplayer — `fl-server` dedicated binary**
- `fl-server`: headless sim server; authoritative game state; runs the same sim tick as
  the client but with no rendering. Clients send inputs; server sends world state diffs.
- ENet (reliable UDP with congestion control) replaces raw BSD sockets for game traffic.
- Target: 32 players per server instance (vs. FA's ~8).
- `fl-lobby` (optional, self-hostable): lightweight REST service for session listing,
  matchmaking, and server registration. Players can use community-hosted or run their own.
- STUN / relay support via ENet for NAT traversal without port forwarding.

**Game modes**
- *Dogfight*: free-for-all or team; score = kills; ends on kill limit or timer.
- *Team Dogfight*: two sides; respawn allowed; team score tracked.
- *Cooperative Strike*: all players vs. AI; shared objectives from mission YAML;
  one player can act as AWACS/tactical coordinator (no aircraft, map view only).
- *Versus Strike*: one team flies AI-defended strike missions; other team intercepts.
  Score: attackers earn points for objectives destroyed; defenders earn points for kills.
- *Campaign Co-op*: players fill named slots in a campaign mission YAML; pilot saves
  are individual but campaign progression is shared on the server.
- *Custom / Scenario*: any mission YAML uploaded to server; mode inferred from mission
  triggers; used for community-created scenarios and tournaments.

**Spectator mode**
- Join server as spectator (no aircraft slot required).
- Cycle through any player's cockpit or chase camera; free camera available.
- Read-only HUD showing all player positions on minimap.

**In-game server browser**
- Lists all servers registered with fl-lobby: name, game mode, map, players/max, ping.
- Filter by mode, map, ping range, password-protected, **and by mod compatibility**
  (toggle: "show only servers I can join without downloading anything").
- Direct IP join for servers not registered with any lobby.

**Server mod advertisement**
- When fl-server registers with fl-lobby it includes its full mod stack: mod ID, version,
  required/optional flag, and a download source for each pack:
  ```json
  "mods": [
    { "id": "fa-content",           "version": "1.2.0", "required": true,  "source": "community-index" },
    { "id": "extra-aircraft-pack",  "version": "0.5.1", "required": true,  "source": "https://host/extra-aircraft.flmod" },
    { "id": "hd-texture-pack",      "version": "2.0.0", "required": false, "source": "community-index" }
  ]
  ```
- Server browser shows a mod badge per listing: green (all installed), yellow (missing
  optional), orange (missing required — download needed), red (version conflict).

**Auto-install on join**
When the player clicks Join on a server with missing or outdated required mods:
1. Client diffs server mod list against local installs; lists missing/outdated packs with
   names, versions, and download sizes.
2. Dialog: "This server requires [N] mod(s) — [X MB total]. Download and install?"
3. On confirm: downloads run in parallel with a progress bar; content stack reloads on
   completion; join proceeds automatically.
4. **Mod sourcing priority**: community index first (by ID + version); server-provided
   URL as fallback. If sourced from a server URL rather than the trusted community index,
   the dialog shows a warning: "Not from the community index — install anyway?"
5. Already-installed mods at the correct version are skipped; no re-download.
6. Optional mods (e.g. HD texture pack) are offered separately after the required-mod
   install completes, with a "Skip" option.

**Mod version management**
- Multiple versions of the same mod coexist on disk (different servers may pin different
  versions); stored as `mods/<id>/<version>/`.
- **Manage mods screen** (Settings → Mods): installed mods with version, source, disk
  size, which recently joined servers required it, and a Remove button.
- Optional auto-cleanup: prompt to remove server-required mod versions that haven't been
  used in N days.

**Persistent multiplayer world**
- `fl-server` supports a `--persistent` mode: campaign state (frontline, unit counts,
  destroyed airfields, pilot contribution log) survives between player sessions.
- Players connect, fly sorties, disconnect; the dynamic campaign continues evolving on
  the server. Other players' actions shift the world the next player logs into.
- Server operators configure reset schedules (e.g., restart campaign every 30 days) and
  starting conditions via a server-side `campaign.yaml`.
- Per-pilot contribution tracking: kills, objectives completed, territory influenced —
  stored in server-side pilot records; accessible in the lobby browser.
- A Game Master can be permanently assigned to a persistent server for ongoing scenario
  management.

**In-game voice and text chat**
- **Text chat**: always available; channels: All / Team / Whisper. Shown as HUD overlay;
  scrollable history in a chat panel. Works for players without a microphone.
- **Voice chat**: built-in via SDL3 microphone capture + ENet audio channel (compressed
  Opus, same transport as game traffic).
  - *Proximity chat*: players within a configurable radius (default 5 nm) hear each other
    regardless of faction — adds immersion and allows trash-talk. Server operator sets
    radius or disables proximity entirely.
  - *Team channel*: push-to-talk (configurable key) broadcasts to all same-faction players
    on the server regardless of position.
  - *Command net*: optional third channel for flight lead / AWACS coordinator; invite-only
    per server role assignment.
  - Voice activity detection (VAD) or push-to-talk — player's choice in settings.
  - Server operator can disable voice entirely (text-only mode).

**Anti-cheat**
Server-authoritative model is the primary defence: the server validates all physics,
weapon fire, and damage state. Inputs that would produce physically impossible results
(impossible turn rate, instant position change) are rejected and the client is warned.
No client-side binary attestation or memory scanning. Combat flight sims attract a
smaller, less adversarial player base than FPS games; re-evaluate if community reports
indicate a real problem post-launch.

**FA compatibility path (parity mode only)**
- FA's 66-packet IPX-over-UDP protocol implemented inside the fa-content bridge.
- Allows connecting to original FA LAN games when running in classic/parity mode.
- Not used by native multiplayer; the two stacks are completely separate.

### 2.8 Vulkan Renderer — Modern Mode (weeks 17–24)
- **Mesh pipeline**: `AssetManager.loadMesh()` returns `MeshData`
  (GPU vertex/index buffers). Damage/LOD state selects correct variant.
- **GPU particle system**: compute-shader particles for explosions, fire, smoke, chaff,
  contrails, debris, craters, hulks, flares, bullets. Spawn params driven by entity
  damage state and Lua event triggers; FA bridge translates FA effect calls to engine
  particle spawn events.
- **Terrain**: `TerrainData` streaming chunk loader; chunks triangulated and uploaded to GPU
  on demand as player moves; no full-map load; arbitrary theater size.
- **Sky / atmosphere**: layered atmosphere system in shaders; fog, sun disc, lens flare, clouds.
- **HUD**: 2D overlay; gauge positions from native HUD layout (`config/hud_layout.toml`);
  glyph bitmaps from content pack PNG atlases.
  Enemy/threat labels: toggleable icons over all entities (friend/foe/neutral color-coded);
  range and closure rate shown on lock; toggleable per difficulty preset.
  **HUD customization**: layout saved to `config/hud_layout.toml`; elements are: radar
  scope, RWR strip, fuel gauge, airspeed, altitude, G-meter, weapon select, minimap,
  objective list, wingman status sidebar. Each element has position (normalized screen
  coords), scale, and opacity. Drag-and-drop editor in Settings → HUD Layout. Named
  presets: Minimal / Standard / Full; mods can ship their own preset layouts.
  Colorblind-friendly palette option: replaces friend/foe/neutral colors with
  shape-coded icons (circle/square/diamond) so color is never the only differentiator.
- **Textures**: RGBA from `AssetManager.loadTexture()`; uploaded to GPU as KTX2 or
  RGBA image depending on content pack output.
- **Weather rendering**: overcast cloud layer, rain particle effect, fog distance falloff,
  night/dusk/dawn ambient lighting. Driven by weather state set in mission YAML.

### 2.9 Carrier Entity System (weeks 18–20)
- `CarrierEntity`: moving platform; position and heading tracked by sim; deck surface is
  a physics plane in the ship's local frame.
- **LSO audio callouts**: "on glideslope", "high/low", "fast/slow", "wave off" — played
  inside 3 nm on approach. Callouts are OGG audio assets; content packs provide voice sets.
- **Deck lighting**: night carrier ops use coloured deck lights and meatball illumination;
  no runway lights on a carrier — orientation is harder without labels (by design).
- **Carrier flight model data** (TOML fields added to aircraft):
  ```toml
  [carrier]
  capable        = true
  approach_kts   = 135      # on-speed AoA target
  cat_min_kts    = 130      # minimum cat-launch airspeed
  hook_length_ft = 17.5
  ```
- FA content bridge: F-14 and F/A-18 carrier spawns in FA missions translated to carrier
  entity start positions; FA carrier model (if present in LIBs) loaded as carrier mesh.

### 2.10 Weather & Time of Day (weeks 20–21)
- Weather state machine: `clear → overcast → rain → storm`; transitions over time or
  triggered by mission YAML.
- Time of day drives ambient light, sun angle, shadow direction, and horizon color.
  Dusk/dawn transitions are gradual; missions can start at any hour.
- Gameplay effects (scaled by difficulty):
  - Overcast / rain: reduced visual range; IR seekers lose lock in heavy cloud.
  - Storm: turbulence (flight model perturbation), radar rain clutter reduces detection range.
  - Night: enemy labels suppressed unless NVG or radar lock; AI search range reduced.
  - NVGs: toggle-able cockpit overlay; green-tinted; available to aircraft that carry them.
- Weather is the same for all players in multiplayer (server-authoritative weather state).
- FA bridge: FA's atmosphere layer data translated to equivalent weather state on mission load.

### 2.11 Instant Action / Quick Play (weeks 21–22)
Quick configuration screen; no campaign, no pilot impact.
- **Dogfight**: choose map, time of day, weather, player aircraft, enemy count and type.
  Enemies spawn with AI wingmen at configurable difficulty. Score tracked for session only.
- **Strike**: choose map, player aircraft, target category (airfield / SAM site / convoy /
  naval). Objectives and enemy defenses generated from templates.
- **Free flight**: choose map, aircraft, weather, time. No enemies; practice area.
  Useful for HOTAS calibration and training.
- Instant action scores feed an optional local leaderboard; not linked to pilot campaign save.

### 2.12 Replay System + Photo Mode (weeks 22–23)
- Every mission (campaign, instant action, multiplayer) recorded automatically to a replay
  file (`replays/<timestamp>_<mission>.flrep`).
- **Playback**: any camera — cockpit, external chase, padlock, free orbit, cinematic path.
  Scrub timeline; 0.25× / 0.5× / 1× / 2× speed.
- **Photo mode**: pause playback → free camera → adjust FOV, roll, exposure → export PNG.
  Available during live missions too (pauses sim in single-player).
- Replay files are shareable; engine version-stamped; forward-compatible within a major version.
- Multiplayer replay: recorded server-side; all player tracks included; any player's
  perspective can be viewed in playback.

### 2.13 Radar, Weapons & Electronic Warfare (weeks 23–25)

**Radar system** (per-aircraft TOML `[radar]` block; scaled by difficulty preset):
- *Simple* (Cadet): always-on awareness ring on HUD; threats shown regardless of aspect or range.
- *Standard* (Pilot): selectable radar modes — Search (wide scan), TWS (track-while-scan,
  multiple soft tracks shown on scope), STT (single-target track, full guidance), Bore
  (boresight cone for close-in dogfight). Lock range and scan rate from aircraft TOML.
- *Full* (Ace): radar has sweep delay (targets appear after beam passes them), notch
  vulnerability (target flying perpendicular drops off scope), rain clutter reduces range
  in storm weather.
- **RWR (Radar Warning Receiver)**: all difficulty levels. Threat library loaded from
  `data/rwr_threats.toml`; each emitter entry carries band, type (search/track/fire-control),
  and launch-warning flag. RWR displays direction strip + audio tone; pitch/urgency scales
  with threat type.

**Weapon guidance** (driven by weapon TOML `[seeker]` block):
- *Active-radar* (AIM-120 class): fire-and-forget; seeker acquires independently after
  launch; chaff + notch degrades probability of kill.
- *Semi-active-radar* (AIM-7 class): requires continuous radar lock until impact; breaking
  lock breaks guidance.
- *IR — all-aspect* (AIM-9X class): locks any aspect; clouds and sun disc can break lock;
  flares degrade by susceptibility value.
- *IR — rear-aspect* (older AIM-9 class): front-quarter lock prohibited; flare-susceptible.
- *Laser-guided* (GBU-12 class): requires continuous laser designation from player or
  wingman; designation broken by target obscuration or player maneuver.
- *Unguided*: pure ballistic; CCIP (continuously computed impact point) pipper on HUD
  at all difficulty levels so aiming is skill-based, not chart-based.
- *Anti-radiation* (HARM class): homes on active radar emitters from RWR threat list;
  emitter shutdown causes missile to go ballistic (misses); partial effectiveness.

**Electronic countermeasures**:
- Aircraft ECM pod (TOML `[ecm]` block, `jam_strength` 0–1): reduces enemy radar
  acquisition range proportionally; burn-through zone at close range regardless of jamming.
- Chaff: dispensed in bursts; breaks radar missile lock probabilistically
  (`chaff_susceptibility` from weapon TOML); less effective if missile is in terminal phase.
- Flares: breaks IR missile lock (`flare_susceptibility` from weapon TOML); ineffective
  against radar-guided missiles.
- All countermeasure quantities tracked per sortie; resupply only on ground.

**RWR threat library** (`data/rwr_threats.toml` — moddable):
```toml
[[emitters]]
id             = "sa10_search"
name           = "SA-10 Search"
band           = "C"
type           = "search"
launch_warning = false

[[emitters]]
id             = "sa10_fcr"
name           = "SA-10 Fire Control"
band           = "X"
type           = "fire-control"
launch_warning = true
```

### 2.14 Sandbox Mode & Game Master (weeks 25–27)

**Sandbox mode** — first-class main menu entry alongside Campaign, Instant Action, and
Multiplayer. Not "campaign with no story" — a distinct, explicitly open-ended mode.

- Configure before launch: map, factions, starting force balance, unit counts per side,
  time of day, weather preset, and optional win condition (or none — play indefinitely).
- Dynamic campaign layer runs with no story overlay: pure ground-war simulation generating
  sorties until the player ends the session or a win condition is met.
- Session saves and resumes from any point; no mission-boundary checkpoints required.
- All aircraft, weapons, and maps available; player sets their own loadout constraints.
- Can transition to multiplayer mid-session: host the current sandbox state as a server;
  other players join into the running world.

**Game Master mode** — available in sandbox (single-player) and as a designated role in
multiplayer. The Game Master has no aircraft; they operate from a live map view while the
sim runs. Intended for cooperative table-top-style play, dynamic scenarios, and rapid
mission design iteration.

- **Top-down map view**: full terrain overview; all entity positions shown in real time.
- **Spawn**: place any entity type anywhere — aircraft (AI or empty slot for a player to
  join), ground vehicles, ships, SAM sites, structures. Spawned entities are live
  immediately.
- **Despawn**: remove any entity; AI aircraft receive a "return to base" command before
  despawn to avoid abrupt disappearance.
- **Weather control**: change weather state in real time; transitions play out over the
  configured transition duration.
- **Objective editor**: place trigger zones, set conditions and actions from the visual
  trigger editor — all changes take effect immediately in the running session.
- **Radio**: send text or audio message to all players or a specific player.
- **Time control**: advance time of day; adjust time compression (single-player only).
- In multiplayer, Game Master is assigned by the server host; one GM per server.
- The Game Master surface is the in-game mission editor (Phase 4.2) running in live mode —
  the same tool, different execution context.

### 2.15 Difficulty & Accessibility Settings (weeks 26–27)
Named presets saved per pilot profile; all individual toggles overridable.
In sandbox mode all settings default to the player's profile preset but can be changed
at any time mid-session — the game never forces a difficulty.

| Setting | Cadet | Pilot | Ace | Notes |
|---|---|---|---|---|
| Flight assists | All on | G-limiter only | All off | Auto-level, G-limit, auto-throttle |
| Aim assist | On | On | Off | Missile snap, gun lead |
| Invulnerability | Optional | Off | Off | Toggle in session |
| Unlimited weapons | Optional | Off | Off | Toggle in session |
| Enemy labels | Always | On lock | Off | Range / closure shown on lock |
| Radar realism | Simple | Standard | Full | Simple = always-on awareness ring |
| Blackout / redout | Off | On | On | G-force vision effects |
| Fuel consumption | Off | On | On | |
| In-flight refueling | Auto | Simplified | Manual | See Phase 2.3 tiers |
| Friendly fire | Off | Off | On | |
| Crash damage | Off | On | On | Ground / water impact |
| Rearm mode | Instantaneous | Timed (60–120 s) | Supply-limited | Campaign YAML can override |

**Enemy AI scaling by difficulty preset** — the table above controls player-side
experience; the preset also governs how enemy AI behaves:

| AI parameter | Cadet | Pilot | Ace |
|---|---|---|---|
| Reaction time | 1.5 s | 0.8 s | 0.3 s |
| Aim error (cone half-angle) | 8° | 4° | 1° |
| Radar / sensor range | 50% | 80% | 100% |
| Countermeasure use | Never | Reactive | Proactive |
| Energy management | Passive | Standard | Aggressive BFM |
| SAM engagement range | 60% | 80% | 100% |
| SAM radar shutdown (HARM evasion) | Never | Sometimes | Always |

All values are data-driven multipliers in `data/difficulty.toml`; mods can tune them.
In multiplayer the server's declared minimum preset sets the floor for AI opponents;
human players always play against the server's AI difficulty regardless of their own preset.

Multiplayer servers declare a minimum difficulty preset; players at higher difficulty
are always accepted. In competitive modes, Ace settings are enforced server-side.

## Phase 3 — Classic / Parity Mode
**Duration: 10–14 weeks (starts week 24)**
**Milestone: Exact visual parity with original FA at any resolution**

Second render path behind the same HAL. Toggle at launch.

**Classic mode and non-FA content**: Classic mode renders any `MeshData` returned by
`AssetManager.loadMesh()` — it does not care whether that data came from the FA bridge
or a native glTF pack. A native content pack that ships geometry in the right vertex
format will render through the software rasterizer and look like an FA-era game. This is
intentional: it gives modders a retro aesthetic option without requiring FA assets.
However, the software rasterizer requires the 8-bit VGA palette from the active content
pack (`palette.pal` at the pack root); a native pack that omits it falls back to a
default greyscale palette. Native content that ships GPU-only assets (e.g. KTX2 textures
with no 8-bit equivalent) will render with missing textures in Classic mode — this is
acceptable and documented. Classic mode is therefore FA-optimal but not FA-exclusive.

### 3.1 Full SH Bytecode Interpreter (weeks 24–28)
Implement all opcode handlers from C.2: `JumpToLOD`, `JumpToDamage`, `JumpToFrame`,
`XformUnmask`, `Unmask`, `SourceName`, all confirmed `Unk*` opcodes.

### 3.2 Software Rasterizer Path (weeks 26–30)
- Painter's sort (no z-buffer); centroid + size-bias sort key.
- Integer polygon fill (fixed-16.16, Gouraud variant).
- NPM float triangle path (perspective-correct texture mapping; 7-float FVERTEX).
- WR atmosphere: 8-bit 192-entry VGA palette; per-frame animation; blacken/whiten/redden.
- Texture remap cache (8-entry LRU, 64-entry `_tmapRemapTable`).
- Horizon sequence: `_SolidHorizon` → `@G_Tile@32` → `_GouraudHorizon` → `_GRExec_4`.
- Render to 8-bit framebuffer; resolve to RGBA for Vulkan swapchain present.

### 3.3 x86 Effect Interpreter (weeks 28–34)
- Instrument FA.EXE to log instruction forms across all 65 effect files.
- Implement minimal x86 interpreter (expected subset: ~300–400 distinct encodings).
- Feed entity state into interpreter's virtual register file.
- Files: `FIRE.SH`, `FLARE.SH`, `BULLET.SH`, `CHAFF.SH`, `CLOUD*.SH`,
  `CRATER.SH`, `DEBRIS.SH`, `EXP.SH`, `EJECT.SH`, `AC130.SH` + ~55 others.

## Phase 4 — ft-gui Cross-Platform Port + In-Game Mission Editor
**Duration: 10–12 weeks (starts week 26, after engine HAL is stable)**
**Milestone: fighters-toolkit modding GUI runs on Windows, Linux, macOS;
in-game mission editor ships as part of Fighters Legacy**

### 4.1 ft-gui Cross-Platform Port (weeks 26–32)
- Replace `imgui_impl_dx11.cpp` with `imgui_impl_vulkan.cpp` (engine's VkRenderer HAL).
- Replace `imgui_impl_win32.cpp` with `imgui_impl_sdl3.cpp`.
- Replace `waveOut` audio with OpenAL Soft.
- Extend ft-gui to work with **both FA formats and native open formats**:
  - "New aircraft" wizard: create TOML flight model + glTF model + OGG audio.
  - glTF mesh viewer (replaces the DX11 SH viewer from Phase 0.2).
  - TOML flight model editor (all fields with units, validation).
  - YAML campaign graph editor (visual node graph; click missions to edit).
- Test all editor panels on Ubuntu and macOS.

### 4.2 In-Game Mission Editor (weeks 30–38)
A map-based editor accessible from the main menu and from within replays.
No text file editing required; exports standard mission YAML.

- **Map view**: 2D top-down rendering of the terrain chunk grid; zoom + pan; grid overlay.
- **Object palette**: tabbed sidebar — Aircraft, Ground, Naval, Structures, Waypoints,
  Trigger Zones. Click to place; drag to reposition.
- **Property panel**: edit selected entity — type, side, heading, altitude, starting speed,
  AI script assignment, hardpoint loadout.
- **Waypoint editor**: click to place waypoints; link to entities by drag; set altitude,
  speed, and action per waypoint (orbit, attack, land, RTB).
- **Trigger editor**: select source (entity / zone / timer), select event (destroyed /
  entered zone / time elapsed), select action (mission success / spawn / radio message /
  weather change / unlock objective). Trigger chains are Lua under the hood; visual editor
  generates the Lua automatically.
- **Test launch**: immediately start the current editor state as a playable mission without
  saving; returns to editor on exit.
- **Export**: saves as standard mission YAML; immediately loadable by instant action or
  campaign slot.
- **Import**: open any existing mission YAML (including FA bridge output) into the editor.
- Shipped as part of the Fighters Legacy client; not a separate tool.
- **Dual execution context**: the same editor runs as a pre-mission designer (standard use)
  or as a live Game Master surface during a running session (Phase 2.14). The distinction
  is only whether the sim is paused or running — the UI is identical.
- ft-gui's YAML mission text editor remains available for power users and version-control
  workflows.

## Phase 5 — Linux / Mac Release
**Duration: Ongoing from day one; formal milestone at ~week 40**
**Milestone: fighters-legacy and fighters-toolkit ship official Linux + macOS binaries**

- CI matrix: `windows-latest` (MSVC), `ubuntu-24.04` (GCC/Clang), `macos-14` (arm64).
- MoltenVK verified on Apple Silicon M-series.
- x86 effect interpreter: pure software, runs on arm64 and x86_64.
- FA bridge: LIB archive entry names normalized to lowercase at mount time (Linux case-sensitivity).
- README: users must provide their own FA installation; no game assets redistributed.
- Community translation PRs accepted; first contributed locales (likely DE, FR, ES, RU)
  included in the Phase 5 release if submitted before the release branch cut.

## Phase 6 — Native Open Formats + Free Base Pack
**Duration: 8–12 weeks specification + tooling; free base pack is an ongoing community effort**
**Milestone: Fighters Legacy playable with no FA dependency**

### 6.0 In-Game Mod & Mission Browser
Discovery and installation happen inside the game — players never need to touch the
filesystem to install community content.

- **Content index**: a community-hosted index file (JSON, served over HTTPS) listing
  available mods, missions, campaigns, and aircraft packs with metadata, screenshots, and
  versioned download URLs. The index URL is configurable; server operators and communities
  can run their own private indexes.
- **In-game browser**: browseable grid/list with category filters (aircraft, missions,
  campaigns, maps, full mods), screenshots, descriptions, and one-click install.
- **Install**: downloads `.flmod` file, extracts to `mods/<id>/<version>/`, scans manifest,
  adds to content stack. Missions and campaigns are available immediately; aircraft and
  maps require a session restart.
- **Updates**: browser shows installed mods with version badges; one-click update to latest.
  Servers that pin an older version keep their pinned version; only the browsed/default
  version updates.
- **Share from editor**: mission editor has a "Publish" button — packages the mission as
  a `.flmod`, prompts for description and screenshot, uploads to a community submission
  endpoint.
- **Shared sourcing backend**: the server browser auto-install flow and the mod browser
  install flow use the same `ModDownloader` component — same progress UI, same version
  coexistence logic, same trust warnings. Installing a mod from the browser and having it
  auto-installed when joining a server are the same operation.
- **Lua sandbox security**: all Lua in mods runs in a restricted sandbox — no filesystem
  access, no network calls, no FFI. The sandbox exposes only the documented `ai`, `world`,
  and `mission` API surfaces. This applies equally to manually installed mods and
  auto-installed server mods; no elevated trust is granted based on install source.
- The index and submission service are community-operated; the game ships configured to
  point at the community default but any URL works.

### 6.1 Native Format Toolchain
- `tools/pack-textures/` — PNG → KTX2 batch converter (uses `toktx` / `compressonator`).
- `tools/validate-mod/` — lints manifest.toml + checks all referenced assets exist.
- `tools/pack-mod/` — zips a mod directory into a `.flmod` distributable.
- ft-gui extended (Phase 4) with native format editors covers the authoring side.

### 6.2 Modding Documentation
- `docs/modding/getting-started.md` — install structure, first mod tutorial.
- `docs/modding/3d-models.md` — Blender → glTF workflow, LOD setup, damage meshes.
- `docs/modding/flight-models.md` — TOML field reference (all fields with units).
- `docs/modding/missions.md` — YAML schema reference; triggers, conditions, objects.
- `docs/modding/terrain.md` — heightmap format, surface classes, texture atlas.
- `docs/modding/ai-scripts.md` — Lua AI API reference; `ai` / `world` / `mission` module docs; tutorial scripts; FA goto-script migration guide.
- `docs/modding/audio.md` — OGG spec, positional audio setup, music playlist format.

### 6.3 Free Base Pack (Community Content)
A standalone content repository (`fighters-legacy-base-pack` or similar) providing
a fully open-license set of assets playable without FA:

| Asset category | Approach |
|---|---|
| Aircraft 3D models | Community-created in Blender; exported as glTF |
| Aircraft textures | Community-created PNG under CC-BY |
| Flight model data | TOML files tuned from open aviation performance databases |
| Terrain | Heightmaps derived from SRTM elevation data; hand-painted surface classes |
| Missions | YAML files authored from scratch (no FA content) |
| Audio | CC0 / CC-BY sound effects; FluidSynth-rendered music from public domain MIDI |
| AI scripts | Lua scripts using the engine's `ai` API; FA goto-script behaviors used as behavioral reference only |

The free base pack is not a deliverable of this repo — it is a community effort enabled
by this phase's tooling and documentation.

### 6.4 Ground Crew Scene + Base Operations
High-immersion post-landing experience. Engine support ships in Phase 6; art and audio
assets are a community content effort, contributed alongside or after the free base pack.

**Engine support (Phase 6):**
- Landing detection triggers a blend to an external camera orbiting the aircraft on the
  ramp; the sim continues at 0× time compression during the base operations screen.
- **Base operations menu**: accessible from the ground crew scene:
  - *Rearm loadout*: same hardpoint editor as the pre-mission screen; change weapons per
    slot; confirms into the next sortie.
  - *Refuel*: slider showing current and target fuel load (matters in `timed` and
    `supply-limited` rearm modes).
  - *Repair status*: damage threshold indicators per system (airframe / engine / avionics
    / controls); repair time shown if `timed` rearm mode is active.
  - *Return to campaign map* or *launch next sortie* buttons.
- Audio event hooks: crew chief dialogue plays as positional audio from the aircraft
  during the scene; content packs bind OGG clips to landing, rearm-complete, and
  launch-ready events.
- Scene exits automatically when the player launches the next sortie or after an
  idle timeout (configurable; defaults to 60 s).

**Community art assets (Phase 6+ / free base pack):**
- Ground crew glTF models with servicing animations: fuel truck approach and hose
  connect, weapons cart with crew loading pylons, crew chief with wand signals.
- Crew chief OGG dialogue library: landing acknowledgment, rearm status callouts,
  launch clearance ("Good hunting, cleared for takeoff").
- Aircraft-specific ramp positions and crew placement defined in aircraft TOML:
  ```toml
  [ground_crew]
  crew_chief_pos = [2.5, 0, -3.0]   # offset from aircraft origin
  fuel_truck_pos = [-4.0, 0, 1.5]
  scene_camera   = "ramp_right"      # named camera anchor in cockpit glTF
  ```
- Content packs that lack ground crew assets fall back to the base operations menu
  only (no external scene), so the feature degrades gracefully.

## Schedule Summary

| Phase | Weeks | Key Dependency |
|---|---|---|
| 0 — Toolkit completion | 1–4 | Gameplay access for PLT differential save |
| 1A — Engine + content system | 1–10 | None (start immediately) |
| 1B — FA content bridge | 2–8 | Phase 1A IContentPack interface |
| 1C — RE gap closure | 1–14 | Ghidra + FA.EXE; audit OpenFA first |
| 2 — Modern-particles engine | 10–36 | HAL + FA bridge stable; SH opcodes closed by week 18 |
| 3 — Classic/parity mode | 28–42 | All C.2–C.5 RE closed; Phase 2 running |
| 4 — ft-gui + in-game editor | 26–38 | Phase 2 Vulkan HAL stable |
| 5 — Linux/Mac release | ongoing → ~44 | All CI green; MoltenVK verified |
| 6 — Open formats + free pack | 36–50 | ft-gui extended; modding docs complete |

Total estimated duration: **~52–60 weeks** of focused work.
Phase 2 expanded for dynamic campaign, carrier ops, weather, replay, difficulty system, sandbox mode, and Game Master.
Phase 6 expanded for in-game mod browser and Lua mission template extension API.

## Critical Path

1. **SH opcode RE (C.2) → SH renderer (2.8) → Parity mode (3.1/3.2)**
   Longest sequential chain. Audit OpenFA first to shorten.

2. **PT_TYPE byte offsets (Phase 0.4) → FA bridge flight model translation (2.3)**
   PT_TYPE offsets are needed by the FA bridge to correctly populate FlightModel from
   FA's BRF data. Must close before Phase 2 flight model work starts with FA content.

3. **IContentPack interface (A.3) → FA bridge (1B) → all Phase 2 asset loading**
   Interface must be stable before the bridge and engine subsystems depend on it.
   Lock the interface in week 2; do not break it after that.

4. **Weapon TOML format → loadout screen (2.5) → dynamic campaign unit pools (2.5)**
   Weapon IDs must be stable before the loadout screen, AI weapons selection, and
   dynamic campaign mission templates can reference them. Lock weapon TOML schema
   before Phase 2.3 flight model work starts (hardpoints reference weapon IDs).

5. **Ground/naval unit TOML format → dynamic campaign (2.5) → sandbox unit spawning (2.14)**
   Unit IDs and AI script contracts must be defined before the dynamic campaign generator
   can instantiate unit pools or the Game Master spawn palette can list unit types.

6. **Radar & EW (2.13) → AI weapons selection (2.4) → multiplayer balance (2.7)**
   AI must know missile guidance types to select correct weapons and respond to jamming.
   EW parameters affect game balance in multiplayer; lock before server-side enforcement
   of Ace difficulty settings.

## Verification

### Toolkit (Phase 0)
- `ctest` passes all Catch2 round-trip codec tests.
- PLT stats fields match values visible in FA's in-game pilot screen.

### Engine Foundation (Phase 1)
- All three CI jobs build clean.
- Vulkan validation layers: zero errors on triangle hello-world.
- MoltenVK smoke test passes on Apple Silicon.
- FA content bridge loads a single `.SH` mesh and displays it in a debug window.

### Modern-Particles Engine (Phase 2)
- FA mission `U01.M` loads and runs to completion via FA bridge.
- Flight model stall speed + fuel burn match design spec for each aircraft type.
- Radar lock, missile fire, and chaff defeat sequence works at all three difficulty tiers.
- AIM-120 fire-and-forget and AIM-7 continuous-lock guidance both function correctly.
- RWR triggers on SA-10 track radar; launch warning sounds; HARM homes on active emitter.
- Progressive damage: light / heavy / critical thresholds produce correct visual + penalty.
- Carrier landing: wire catch at correct speed; bolter triggers go-around correctly.
- In-flight refueling: all three tiers (auto / simplified / manual) functional.
- Wingman commands: all six commands acknowledged and executed by AI.
- Dynamic campaign: frontline advances after objective completion; story mission injects at trigger.
- Sandbox mode: configurable start, no win condition, session saves and resumes.
- Game Master: entity spawn/despawn and weather change take effect in running session.
- Replay: mission records and plays back from cockpit and free-camera views.
- GPU particles render for explosion / smoke / fire.
- Multiplayer: two clients on fl-server complete a cooperative strike mission.
- CI green on all three platforms.

### Classic/Parity Mode (Phase 3)
- Screenshot comparison: classic mode vs. original FA at matching camera angles.
  No structural geometry differences.
- All 1,275 SH files render; damage/LOD states visually correct.
- 65 x86-only effect files produce matching particle geometry.

### ft-gui Cross-Platform (Phase 4)
- All editor panels work on Ubuntu and macOS.
- Round-trip: create a TOML aircraft + glTF mesh; load it in the engine.

### Open Formats (Phase 6)
- A mission using only native YAML + glTF + TOML + OGG content runs without
  the FA content bridge installed.
- `validate-mod` passes on the free base pack when available.

## Distribution & Monetization

### License and Charging

GPL v3 permits selling the software. Recipients receive the source and may redistribute it
freely — this is expected and not a problem. The model is identical to how id Software
ships Quake engines on Steam: GPL code, community redistribution, commercial distribution
through storefronts. Proprietary game data (the FA content bridge, community-created
asset packs) is unaffected by the engine license.

**Critical constraint**: the Steamworks SDK is proprietary. Linking it into a GPL binary
creates a license conflict. Fighters Legacy must not link Steamworks — Steam is used
purely as a delivery vehicle (installer, auto-update), not as an integrated SDK.

### Distribution Channels

The five channels below are **additive and non-exclusive**. All can be active
simultaneously; the table shows when each makes sense to add.

| Channel | Cut | When to add | Notes |
|---|---|---|---|
| **GitHub Releases** | 0% | Phase 1 alpha | Required — GPL demands public source. Primary source for developers and power users. |
| **itch.io** | 0–10% | Phase 2 early access | Zero approval friction. Best channel for early community. Pay-what-you-want or fixed price. |
| **Flathub** | 0% | Phase 5 (Linux milestone) | Linux desktop packaging via Flatpak. Reaches distro users who don't use itch.io. |
| **Steam** | 30% | After Phase 5 polish | Largest gaming audience. $100 one-time publishing fee. No Steamworks SDK linkage. |
| **GOG** | 30% | Aspirational post-Steam | GOG curates; apply after demonstrable Steam traction. Best for DRM-free audience. |

### Recommended Rollout

1. **Phase 1 alpha** — GitHub Releases only. Source + unsigned binaries. Developer audience.
2. **Phase 2 early access** — Add itch.io. Pay-what-you-want pricing optional. Community feedback loop.
3. **Phase 5** — Add Flathub. Linux Flatpak complements itch.io AppImage.
4. **After Phase 5** — Submit to Steam once the build is polished enough for a general audience.
5. **Post-Steam** — Apply to GOG if Steam reception warrants it.

### What the Free Base Pack Changes

Once the free base pack (Phase 6) is available, Fighters Legacy becomes playable with
zero financial barrier. Revenue model shifts to:
- **FA Content Bridge** as a paid optional plugin (if redistributed as compiled binary)
- **Community packs** priced independently by their creators
- Donations via itch.io / GitHub Sponsors for the engine itself

The engine itself remains GPL and free to compile from source at any time.

---

## Deferred / Future Work

All previously deferred gameplay and architecture items have been resolved and incorporated
into the plan:
- Anti-cheat → Phase 2.7: server-authority only; revisit post-launch on community evidence
- Formation AI positions → Phase 2.4: explicit slot geometry, formations.toml, Lua hold coroutine
- Ground crew scene → Phase 6.4: engine support in Phase 6; art assets as community content

The following are intentionally out of scope for this vision document. They are real
decisions that must be made, but belong in later-stage artifacts:

| Topic | Where it belongs |
|---|---|
| fl-server operator config file format | Phase 2.7 technical spec |
| Community index hosting infrastructure (who runs it, costs, governance) | Community operations doc; decide before Phase 6 ships |
| Minimum system requirements table | Store page copy (Steam, itch.io, GitHub README) |

No open deferred items remain.