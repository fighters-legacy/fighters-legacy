# Native Open Asset Format Specifications

These are the engine's canonical formats. All content packs are expected to provide assets in these formats.

For authoring tools and workflow guides, see the other files in this directory.

---

## 3D Models — glTF 2.0

- Aircraft, vehicles, weapons, buildings, terrain features
- Coordinate convention: right-handed, **+Y up, +X forward**, metres; triangles wound CCW from
  outside (outward normals) — opaque materials are single-sided. See [`docs/modding/3d-models.md`](3d-models.md#coordinate-system-and-winding)
- Damage states: separate glTF meshes or morph targets (`_b` suffix = battle-damaged)
- LOD variants: glTF `LOD` extension or separate files (`F22_lod0.glb`, `F22_lod1.glb`)
- Animations: glTF `animations` array (gear extend/retract, prop rotation, bay doors)
- Shadow mesh: separate `F22_shadow.glb`
- Cockpit interior: optional separate `F22_cockpit.glb`; includes camera anchor point and
  instrument panel geometry. Instruments are non-interactive geometry (no DCS-style clickable cockpit).
- Toolchain: Blender → glTF 2.0 export (see [`docs/modding/3d-models.md`](3d-models.md))

---

## Textures — PNG + KTX2

- Source: PNG (RGBA, any resolution)
- GPU-ready: KTX2 with BC1/BC3/BC7 compression + mipmaps generated at pack time
- Naming: `aircraft_f22.png`, `terrain_grass.png` (lowercase, snake_case)
- Palette-mapped textures from FA are converted to full RGBA at import time

> For the texture pipeline guide including format selection matrix and `tex-compress` usage,
> see [`docs/modding/textures.md`](textures.md).

---

## Audio — OGG Vorbis / Opus

- Sound effects: OGG at 44.1 kHz stereo or mono
- Music: OGG (pre-rendered from MIDI/FluidSynth during content pack build)
- Content packs that use legacy audio formats handle their own conversion before providing OGG to the engine
- Streaming sources (long music tracks) use OpenAL streaming buffers

---

## Music Playlist — TOML

Controls which music track plays in each named game state.

**File location:** `data/playlist.toml` inside the content pack directory.  
Loaded by the engine via `IContentPack::loadConfig("playlist.toml")`.  
Use `validate-playlist --playlist data/playlist.toml [--pack <pack-dir>]` to check it.

**Track names** are asset names passed to `AssetManager::loadAudio()` — no `audio/` prefix and no `.ogg` extension. `FolderContentPack` resolves `"music/patrol_01"` to `<pack>/audio/music/patrol_01.ogg`.

**State IDs** must match the `GameState` enum values: `Menu`, `FlightPatrol`, `FlightCombat`, `MissionSuccess`, `Debrief`.

```toml
# data/playlist.toml
[crossfade]
duration_s = 3.0

[[states]]
id      = "Menu"
tracks  = ["music/menu_theme"]
loop    = true

[[states]]
id      = "FlightPatrol"
tracks  = ["music/patrol_01", "music/patrol_02"]
loop    = true
shuffle = true

[[states]]
id      = "FlightCombat"
tracks  = ["music/combat_01", "music/combat_02"]
loop    = true
shuffle = false

[[states]]
id      = "MissionSuccess"
tracks  = ["music/victory"]
loop    = false
```

**Per-state fields:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `id` | string | — | Must match a `GameState` name: `Menu`, `FlightPatrol`, `FlightCombat`, `MissionSuccess`, `Debrief` |
| `tracks` | string array | — | Asset names passed to `AssetManager::loadAudio()`; no `audio/` prefix, no `.ogg` extension |
| `loop` | bool | `true` | When `true`, restarts from the beginning (or a fresh shuffle) after the last track ends |
| `shuffle` | bool | `false` | When `true`, randomises track order on state entry using Fisher-Yates; no track plays twice before the full cycle completes; the list is re-shuffled on each loop |

State transitions are driven by engine events. Lua scripts can force a state change with `world.set_music_state("FlightCombat")` (deferred to Phase 4 scripting workstream).

---

## Flight Model — TOML

All units are SI. See [`docs/modding/flight-model.md`](flight-model.md) for the complete
authoring guide, including sign conventions, data sources, tuning tips, and worked examples
for the F/A-18C Hornet and Tu-95MS Bear.

```toml
[aircraft]
name         = "F-22 Raptor"
type         = "fighter"       # see flight-model.md for valid values
engine_type  = "turbofan"      # "turbojet" | "turbofan" | "turboprop" | "piston"
has_fbw      = true            # fly-by-wire enforces G/AoA limits even with assists off
cruise_alt_m = 15240           # ~50 000 ft — AI autopilot reference
mesh         = "f22"
cockpit      = "f22_hud"

[flight_model]
mass_kg      = 19700.0         # operating empty + typical payload
wing_area_m2 = 78.0
wingspan_m   = 13.6
mac_m        = 4.9             # mean aerodynamic chord
fuel_kg      = 8200.0          # max internal fuel
ixx_kg_m2    = 22000.0         # roll moment of inertia
iyy_kg_m2    = 160000.0        # pitch moment of inertia
izz_kg_m2    = 175000.0        # yaw moment of inertia

[aero.cl_table]
# rows = alpha breakpoints (deg), cols = Mach breakpoints
alpha  = [-5, 0, 5, 10, 15, 18, 20, 25]
mach   = [0.3, 0.6, 0.9, 1.2, 1.8]
values = [
    -0.20,-0.22,-0.24,-0.18,-0.12,
     0.05, 0.06, 0.07, 0.05, 0.03,
     0.42, 0.47, 0.54, 0.42, 0.29,
     0.78, 0.87, 1.00, 0.78, 0.54,
     1.08, 1.21, 1.39, 1.08, 0.75,
     1.20, 1.34, 1.55, 1.20, 0.83,
     1.12, 1.25, 1.44, 1.12, 0.78,
     0.87, 0.97, 1.12, 0.87, 0.60,
]

[aero.drag_polar]
cd0           = 0.014          # clean configuration
k             = 0.10           # induced drag factor
speedbrake_cd = 0.06           # when speedbrake deployed
gear_cd       = 0.03           # when landing gear extended

[aero.cd_wave]
# Transonic wave drag — omit for subsonic-only aircraft
mach   = [0.75, 0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.20, 1.50]
values = [0.000, 0.007, 0.020, 0.034, 0.030, 0.022, 0.014, 0.006, 0.002]

[aero.moments]
# Pitch (reference length: mac_m)
cm_alpha = -0.72
cm_q     = -12.5
cm_de    = -1.15
# Roll (reference length: wingspan_m)
cl_beta  = -0.085
cl_p     = -0.42
cl_da    =  0.075
# Yaw (reference length: wingspan_m)
cn_beta  =  0.11
cn_r     = -0.14
cn_dr    = -0.055

[aero.limits]
alpha_stall_deg  =  20.0
max_g_structural =   9.0
min_g_structural =  -3.0
max_mach         =   2.25

[aero.controls]
max_elevator_deg = 30.0
max_aileron_deg  = 20.0
max_rudder_deg   = 30.0

[aero.tvc]                     # optional — omit for non-TVC aircraft
min_angle_deg   = -20
max_angle_deg   =  20
slew_rate_deg_s =   5

[engine]
fuel_flow_idle_kg_s = 0.18
fuel_flow_mil_kg_s  = 1.60
fuel_flow_ab_kg_s   = 4.80
spool_time_s        = 4.0

[engine.mil_thrust]
mach   = [0.0, 0.3, 0.6, 0.9, 1.2, 1.5, 1.8, 2.0, 2.25]
alt_km = [0, 3, 6, 9, 12, 15]
values = [
    156.0, 134.0, 112.0,  89.0,  65.0,  40.0,
    164.0, 141.0, 118.0,  94.0,  68.0,  42.0,
    172.0, 148.0, 124.0,  99.0,  72.0,  44.0,
    178.0, 153.0, 128.0, 102.0,  74.0,  46.0,
    172.0, 148.0, 124.0,  99.0,  72.0,  44.0,
    161.0, 138.0, 116.0,  93.0,  67.0,  42.0,
    147.0, 126.0, 106.0,  85.0,  61.0,  38.0,
    138.0, 118.0,  99.0,  79.0,  57.0,  36.0,
    126.0, 108.0,  91.0,  73.0,  53.0,  33.0,
]

[engine.ab_thrust]             # optional — omit for non-afterburning aircraft
mach   = [0.0, 0.3, 0.6, 0.9, 1.2, 1.5, 1.8, 2.0, 2.25]
alt_km = [0, 3, 6, 9, 12, 15]
values = [
    312.0, 268.0, 224.0, 179.0, 130.0,  80.0,
    328.0, 282.0, 236.0, 188.0, 137.0,  84.0,
    344.0, 296.0, 248.0, 198.0, 144.0,  89.0,
    356.0, 306.0, 256.0, 205.0, 148.0,  92.0,
    344.0, 296.0, 248.0, 198.0, 144.0,  89.0,
    322.0, 277.0, 232.0, 185.0, 135.0,  83.0,
    294.0, 253.0, 212.0, 169.0, 123.0,  76.0,
    276.0, 237.0, 198.0, 159.0, 115.0,  71.0,
    252.0, 217.0, 181.0, 145.0, 105.0,  65.0,
]

# [carrier] block — omit for land-based aircraft (F-22 is land-based)
# [refueling] block — omit if aircraft cannot receive fuel (F-22 has boom receptacle)
[refueling]
type          = "boom"
max_rate_kg_s = 3.0

[[hardpoints]]
slot    = 0
type    = "missile"
allowed = ["aim120c", "aim9x"]
default = "aim120c"

[[hardpoints]]
slot    = 4
type    = "bomb"
allowed = ["gbu32", "mk82"]
default = "gbu32"
```

---

## Weapon Data — TOML

Each weapon is a standalone TOML file. Aircraft TOML hardpoints reference weapon IDs.

```toml
# weapons/aim120c.toml — Active-radar air-to-air missile
[weapon]
id       = "aim120c"
name     = "AIM-120C AMRAAM"
type     = "missile"
category = "air-to-air"

[seeker]
type            = "active-radar"
fov_deg         = 60
acquisition_nm  = 20
fire_and_forget = true

[performance]
max_range_nm      = 30
min_range_nm      = 0.5
max_speed_kts     = 2400
motor_burn_time_s = 4.5
max_g             = 30

[warhead]
blast_radius_ft = 50
damage          = 100

[countermeasures]
chaff_susceptibility = 0.4
notch_susceptibility = 0.6

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
type                = "laser"
requires_designator = true

[performance]
standoff_range_ft = 15000
CEP_ft            = 8

[warhead]
blast_radius_ft = 100
damage          = 180

[load]
weight_lb   = 500
drag_factor = 0.020
```

---

## Ground & Naval Unit Data — TOML

```toml
# units/sa10_battery.toml
[unit]
id     = "sa10_battery"
name   = "SA-10 Grumble Battery"
type   = "sam"
mesh   = "sa10"
mobile = false

[armor]
rating = 2
health = 100

[radar]
emitter_id     = "sa10_search"
track_range_nm = 90
can_shutdown   = true

[[weapons]]
weapon_id    = "s300_missile"
max_range_nm = 90
max_alt_ft   = 100000
targets      = ["air"]

[ai]
script = "ai/units/sam_battery.lua"
```

---

## Mission Files — YAML

> For the complete mission authoring guide including all field descriptions, validation rules,
> trigger reference, and worked examples, see [`docs/modding/missions.md`](missions.md).

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

---

## Campaign Files — YAML

```yaml
name: "Forgotten Skies"
version: "1.0"
sides: [nato, russia]
pilot:
  rank_table: ranks/nato_ranks.toml
  persistent_stats: true

dynamic:
  enabled: true
  theaters:
    - id: ukraine
      initial_frontline: frontlines/ukraine_start.png
      ground_units:
        nato:   { armor: 40, infantry: 60, artillery: 20 }
        russia: { armor: 55, infantry: 80, artillery: 30 }
      templates:
        - { type: intercept, file: templates/ukraine_intercept.yaml }
        - { type: cap,       file: templates/ukraine_cap.yaml }
        - { type: strike,    file: templates/ukraine_strike.yaml }
        - { type: sead,      file: templates/ukraine_sead.yaml }

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
```

---

## Terrain — Streaming Heightmap Chunks + JSON

The engine uses a single continuous world terrain. Theaters are geographic regions within it — they are mission conditions, not separate grids.

- **Terrain ID:** `"world"` is the canonical ID used by fl-base-pack. Theater packs override individual chunks at higher mod priority.
- **Chunk size:** 15,360 m (512 intervals × 30 m — native Copernicus GLO-30 resolution, no upsampling required)
- **Chunk format:** 513×513 pixels, 16-bit grayscale PNG
- **LOD levels:** LOD 0 = 513×513 px (30 m/px), LOD 1 = 257×257 px (60 m/px), LOD 2 = 129×129 px (120 m/px)
- **Chunk path convention:** `terrain/<id>/lod<n>/chunk_<x>_<y>.png` — all lowercase, 4-digit zero-padded coordinates (e.g. `chunk_0003_0007.png`). Paths are fully determined by convention; no manifest field needed.

**World manifest (`terrain/world.json`):**

```json
{
  "name": "World",
  "chunk_size_m": 15360,
  "lod_levels": 3,
  "grid_width": 256,
  "grid_height": 256,
  "elevation_scale": 10,
  "textures": {
    "0": "terrain_grass.ktx2",
    "1": "terrain_water.ktx2",
    "2": "terrain_urban.ktx2",
    "3": "terrain_desert.ktx2"
  }
}
```

---

## Theater Manifest — TOML

Theaters define geographic regions within the world terrain. A theater bounds box is used as a mission condition (e.g. leaving the area triggers failure) — it does not restrict engine terrain streaming or player movement. Players can fly anywhere in the world in sandbox mode.

Theater manifests live in `theaters/<id>.toml` inside a content pack.

```toml
[theater]
id     = "ukraine"
name   = "Ukraine"
bounds = { min_x = 500000, min_z = 200000, max_x = 800000, max_z = 500000 }  # meters, world coords
layer  = "ukraine_clear"   # default weather/lighting layer for this theater
```

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier; matches `map:` field in mission YAML |
| `name` | string | Display name |
| `bounds` | table | Rectangular region in world coordinates (meters, right-handed Y-up) |
| `layer` | string | Default weather/lighting preset (references a layer definition) |

---

## Faction Data — TOML

```toml
# factions/nato.toml
[faction]
id    = "nato"
name  = "NATO"
color = "#4488FF"
icon  = "icons/nato.png"

[relationships]
russia = "hostile"
china  = "neutral"
un     = "friendly"
```

Relationship values: `friendly`, `neutral`, `hostile`. Missions and Lua scripts can override at runtime with `world.set_relationship(a, b, state)`.

---

## HUD Layout — TOML

The engine selects a HUD layout per aircraft using the `cockpit` field from the flight model (see Flight Model TOML above). When a content pack does not provide a layout for a given aircraft, the engine falls back to the builtin `FlightHud` (standard IAS / ALT / HDG / THR / FUEL display).

```toml
# config/hud_layout.toml
[layout]
name   = "Standard"
preset = true

[[elements]]
id      = "radar_scope"
pos     = [0.05, 0.60]
scale   = 1.0
opacity = 0.90
visible = true

[[elements]]
id      = "airspeed"
pos     = [0.10, 0.50]
scale   = 1.0
opacity = 1.0
visible = true
```

---

## Rank Table — TOML

```toml
# ranks/nato_ranks.toml
[[ranks]]
id        = "trainee"
title     = "Trainee"
min_score = 0
icon      = "icons/ranks/trainee.png"

[[ranks]]
id        = "lieutenant"
title     = "Second Lieutenant"
min_score = 500
icon      = "icons/ranks/lt.png"

[[ranks]]
id        = "ace"
title     = "Ace"
min_score = 25000
icon      = "icons/ranks/ace.png"
```

---

## AI Scripts — Lua 5.5

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

Each AI entity runs one or more Lua coroutines. The engine resumes them each sim tick. Behaviors can yield, stack, and interrupt cleanly.

> **Lua 5.5 note:** `global` is a reserved keyword in Lua 5.5. Scripts that use `global` as a variable name will fail to load. Rename any such variables before shipping a pack targeting this engine version.
