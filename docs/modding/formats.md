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

A campaign is a **theater graph** with two interleaved mission sources: a *dynamic* generator that
produces sorties from the current frontline state, and a *story* list of authored missions that fire
on triggers. Consumed by the campaign engine (#635).

```yaml
name: "Forgotten Skies"
version: "1.0"
sides: [nato, russia]          # exactly two; index 0 = side A, index 1 = side B (see frontline raster)
pilot:
  side: nato                   # which side the player flies for
  rank_table: ranks/nato_ranks.toml
  persistent_stats: true

dynamic:
  enabled: true
  theaters:
    - id: ukraine              # must match a theater manifest id
      initial_frontline: frontlines/ukraine_start.png
      frontline_grid: { cols: 180, rows: 85 }    # raster resolution — see Frontline Raster below
      ground_units:
        nato:   { armor: 40, infantry: 60, artillery: 20 }
        russia: { armor: 55, infantry: 80, artillery: 30 }
      templates:
        - { role: intercept, file: templates/ukraine_intercept.yaml, weight: 3 }
        - { role: cap,       file: templates/ukraine_cap.yaml,       weight: 2 }
        - { role: strike,    file: templates/ukraine_strike.yaml,    weight: 2 }
        - { role: sead,      file: templates/ukraine_sead.yaml,      weight: 1, requires: enemy_sam }

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
    on_fail:
      retry: true              # default — mission stays pending, dynamic stays locked
```

### Top-level fields

| Field | Type | Description |
|---|---|---|
| `name` | string | Display name |
| `version` | string | Campaign content version (author-managed; not the engine version) |
| `sides` | string[2] | Exactly two faction IDs. **Order is significant** — index 0 is *side A*, index 1 is *side B* in every frontline raster |
| `pilot.side` | string | Which of `sides` the player flies for |
| `pilot.rank_table` | path | Rank table TOML (pack-relative) |
| `pilot.persistent_stats` | bool | Carry kills/losses/flight time across missions |
| `dynamic` | table | Dynamic sortie generation (below); omit or `enabled: false` for a purely scripted campaign |
| `story` | list | Authored missions fired by trigger (below) |

### `dynamic.theaters[]`

| Field | Type | Default | Description |
|---|---|---|---|
| `id` | string | — | Theater manifest ID; supplies the geographic `bounds` the frontline raster covers |
| `initial_frontline` | path | — | Frontline PNG applied when the campaign starts |
| `frontline_grid` | table | — | `{ cols, rows }` — the raster resolution **the campaign declares**. Every frontline PNG in this theater must match it exactly |
| `ground_units` | table | — | Starting order of battle per side (free-form counters; the generator scales force composition from them and decrements them as missions resolve) |
| `templates` | list | — | Mission templates the generator draws from (below) |

### `dynamic.theaters[].templates[]` — what a template is and when it instantiates

A **template** is a mission YAML with holes in it. It is a normal mission file (same schema as
`missions/*.yaml`) plus a `template:` header block declaring which fields the generator fills:

```yaml
# templates/ukraine_strike.yaml
template:
  role: strike
  fills:
    - target_area:  { from: frontline, side: enemy, prefer: contested }
    - ingress:      { from: frontline, side: friendly, min_distance_km: 60 }
    - opfor:        { from: ground_units, side: enemy, scale: 0.15 }
    - player_flight: { size: 2..4 }

name: "Strike — ${target_area.name}"
map: ukraine
# ... the rest is a normal mission file; ${...} refers to filled values
```

| Field | Type | Default | Description |
|---|---|---|---|
| `role` | string | — | What kind of sortie this is (`intercept`, `cap`, `strike`, `sead`, … — a free-form tag; the generator matches on it, the engine does not enumerate it). Renamed from `type` in the original draft, which collided with the `type` field used elsewhere for entity classes |
| `file` | path | — | The template mission YAML |
| `weight` | int | `1` | Relative selection weight among templates whose `requires` is satisfied |
| `requires` | string | — | Optional precondition tag (e.g. `enemy_sam`) — the template is only eligible when the theater state satisfies it |

**Instantiation is at mission-generation time, not on frontline change.** When the player asks for
the next sortie, the generator: (1) reads the *current* frontline raster and `ground_units`,
(2) filters templates by `requires`, (3) picks one by `weight`, (4) resolves each `fills` entry
against the live theater state, and (5) emits a concrete mission YAML that is then loaded by the
ordinary mission parser (#632). A generated mission is therefore a plain mission file — it can be
saved, replayed, hand-edited and validated like any other, which is what keeps the dynamic path
debuggable. Changing the frontline does **not** retroactively alter an already-generated mission.

### `story[]` — authored missions

| Field | Type | Default | Description |
|---|---|---|---|
| `id` | string | — | Unique within the campaign |
| `file` | path | — | Mission YAML |
| `label` | string | — | Display name in the briefing/menu |
| `trigger` | string | — | When it becomes available: `campaign_start`, `after_sorties: N`, `frontline_reaches: <tag>`, or the `next:` of a prior story mission |
| `locks_dynamic` | bool | `false` | See below |
| `on_complete` | table | — | Applied when the mission is **completed successfully** |
| `on_complete.set_frontline` | path | — | Frontline PNG that **replaces** the theater's frontline state |
| `on_complete.unlock` | string | — | Theater ID to unlock (adds it to the dynamic rotation) |
| `on_complete.next` | table | — | The next story mission and its trigger, e.g. `{ after_sorties: 3, id: u02_iron_fist }` |
| `on_fail` | table | `{ retry: true }` | Applied when the mission fails (below) |

### `locks_dynamic` — what it pauses, and what failure does

`locks_dynamic: true` means: **while this story mission is pending, the campaign's dynamic side
stops.** Concretely, for the theater the mission belongs to:

- the generator produces **no** new sorties — the story mission is the only thing the player can fly;
- frontline progression is **frozen** — `ground_units` are not decremented and the frontline raster
  is not advanced by attrition;
- `after_sorties:` counters do not advance (there are no sorties to count).

This exists so a scripted beat cannot be undermined by the simulation moving underneath it: the
frontline the mission was authored against is the frontline the player flies.

**On success:** `on_complete` is applied (frontline replaced, theaters unlocked, `next` armed), the
lock lifts, and dynamic generation resumes from the *new* state.

**On failure:** by default (`on_fail.retry: true`) the mission stays pending and **the lock stays
on** — the frontline does not move, and the player re-flies it. This is the deliberate choice: the
alternative (lift the lock and let the dynamic war grind on while a story beat is unresolved) makes
the campaign's authored spine depend on the player losing. Authors who want a losable beat set:

| Field | Type | Default | Description |
|---|---|---|---|
| `on_fail.retry` | bool | `true` | Mission stays pending and dynamic stays locked |
| `on_fail.set_frontline` | path | — | Apply a *setback* frontline instead of retrying (implies `retry: false`) |
| `on_fail.next` | table | — | Branch to a different story mission on failure (implies `retry: false`) |
| `on_fail.unlock_dynamic` | bool | `false` | Lift the lock and resume the dynamic war despite the failure |

Abandoning a mission (quitting to menu) is **not** a failure — the mission stays pending, unchanged.

### `set_frontline` — application semantics

`set_frontline` replaces the theater's frontline raster **wholesale and instantly**, between
missions. It is not interpolated and there is no blend: the campaign's frontline is a *state*, and a
story beat sets it. Specifically:

- It is applied **between missions**, while no simulation is running, so it cannot move anything
  mid-flight. There are no live entity positions to reset — a mission's units are spawned from the
  mission file when the mission loads, not carried across missions.
- It **replaces**, not merges: the new PNG is the entire new control field. Pixels the author left
  unchanged are unchanged because they were authored that way, not because of any merge rule.
- `ground_units` are **not** reset by it. A story beat that should also change the order of battle
  says so; a frontline change alone moves control, not force levels.
- The next generated sortie reads the new raster, so the dynamic war resumes from the new lines.

---

## Frontline Raster — PNG

The frontline is an **8-bit grayscale PNG** covering the theater's geographic bounds. It is
**independent of terrain tiling** — the pixels do not correspond to terrain tiles, chunks or
quadtree levels. That decoupling is deliberate: terrain resolution is an engine/streaming concern
that changes with the DEM and the maximum tile level, while a frontline is campaign content. Keying
one to the other would mean a terrain re-tile silently invalidating every authored frontline in
every pack.

*(This supersedes the original "1 pixel per terrain chunk, dimensions from the terrain manifest's
grid_width × grid_height" proposal, which was written for the planar chunk grid that #472 removed —
`TerrainManifest` no longer carries a grid at all.)*

**Format**

| Property | Value |
|---|---|
| File type | PNG, 8-bit grayscale, no alpha |
| Dimensions | Exactly the theater's `frontline_grid` `{cols, rows}` — every raster in a theater matches |
| Extent | The theater manifest's geographic `bounds` |
| Pixel (0, 0) | **North-west corner** — `(max_lat, min_lon)`. Row index increases southward, column index increases eastward (standard image orientation over a north-up map) |

**Pixel values**

| Value | Meaning |
|---|---|
| `0` | Unclaimed / no man's land |
| `1`–`127` | **Side A** control (`sides[0]`); brightness = strength (1 = tenuous, 127 = firmly held) |
| `128`–`254` | **Side B** control (`sides[1]`); `128` = tenuous, `254` = firmly held |
| `255` | Contested — both sides present, control undecided |

**Coordinate mapping.** A pixel's centre maps to geographic coordinates by linear interpolation over
the theater bounds, and to world coordinates via `geodeticToWorld()`:

    lon = min_lon + (col + 0.5) * (max_lon - min_lon) / cols
    lat = max_lat - (row + 0.5) * (max_lat - min_lat) / rows

The inverse (world position → pixel) is used for "who controls the ground under this entity?"
queries. Longitude wrap is handled by the engine when `min_lon > max_lon` (antimeridian theaters).

**Choosing a resolution.** `frontline_grid` is a campaign decision, not a terrain-derived one.
Roughly 5–15 km per pixel is the useful band: fine enough that a frontline reads as a line rather
than a staircase, coarse enough that a theater raster stays small and an author can paint it. The
example above (180 × 85 over ~18° × 8.5°) is ~10 km/px.

**Validation rules** (enforced by `validate-campaign`, #784):

- every `initial_frontline` / `set_frontline` / `on_fail.set_frontline` path resolves inside the pack;
- the PNG is 8-bit grayscale;
- its dimensions equal the theater's `frontline_grid`;
- the theater `id` referenced exists and its `bounds` are a valid geographic box.

All 8-bit values are meaningful (0–255 are all defined above), so there is no "pixel out of range"
check — the original issue's third check does not survive contact with a full encoding.

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
bounds = { min_lat = 44.0, min_lon = 22.0, max_lat = 52.5, max_lon = 40.0 }  # degrees, WGS84-ish
layer  = "ukraine_clear"   # default weather/lighting layer for this theater
```

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier; matches `map:` field in mission YAML |
| `name` | string | Display name |
| `bounds` | table | Geographic bounding box in **degrees**: `min_lat`, `min_lon`, `max_lat`, `max_lon` |
| `layer` | string | Default weather/lighting preset (references a layer definition) |

**Bounds are geographic, not planar.** The world is a sphere (the cube-sphere terrain rewrite,
#472), and the engine's world origin sits at the **north pole** in engine coordinates — so a
theater in the mid-latitudes is thousands of kilometres from the origin, where a rectangle in
world X/Z is a badly distorted region on the ground, not a box. Latitude/longitude bounds are
well-defined anywhere on the planet, including across the antimeridian (`min_lon > max_lon` wraps)
and near the poles. The engine converts to world coordinates with `geodeticToWorld()`
(`engine/flight/Geodetic.h`); content never writes raw world metres for a theater.

Latitude is clamped to [-90, 90]; longitude to [-180, 180]. The box may wrap the antimeridian but
may not span more than 180° of longitude in one theater.

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

## Entity Definition TOML

Controls what an entity is: its mesh, HP, damage model, flight physics, and optional default
Lua AI script. Place entity definition files anywhere in the pack directory (typically
`entities/<name>.toml`) and register them via the server's mod loader.

**Required fields:**

| Field      | Type   | Description                                             |
|------------|--------|---------------------------------------------------------|
| `id`       | string | Pack-scoped identifier, e.g. `fl-base:f15c`            |
| `name`     | string | Human-readable display name                             |
| `category` | string | `air_vehicle`, `ground_vehicle`, `naval`, `projectile`, `structure` |
| `max_hp`   | float  | Maximum hit points                                      |

**Optional fields:**

| Field               | Type   | Default | Description                                                |
|---------------------|--------|---------|------------------------------------------------------------|
| `mesh`              | string | `""`    | Primary glTF asset name (renderer)                         |
| `classic_damage_mesh` | string | `""` | Battle-damaged glTF variant (renderer)                     |
| `flight_model`      | string | `""`    | Flight model TOML asset name; empty = builtin UFO model (server-side only) |
| `ai_script`         | string | `""`    | Lua AI script name from the pack's `ai/` directory; auto-assigned when spawned without `--ai`; empty = no scripted AI (server-side only) |

**Example:**

```toml
[entity]
id           = "fl-base:f15c"
name         = "F-15C Eagle"
category     = "air_vehicle"
max_hp       = 300.0
mesh         = "aircraft/f15c"
flight_model = "flight/f15c"
ai_script    = "f15c_patrol"

[damage.light]
hp_fraction    = 0.70
visual_effect  = "smoke_light"
thrust_factor  = 0.85
control_factor = 0.95

[damage.heavy]
hp_fraction    = 0.35
visual_effect  = "smoke_heavy"
thrust_factor  = 0.60
control_factor = 0.75

[damage.critical]
hp_fraction      = 0.10
visual_effect    = "fire"
thrust_factor    = 0.10
control_factor   = 0.30
avionics_failure = true

[classic]
damage_mesh = "aircraft/f15c_dmg"
```

---

## AI Scripts — Lua 5.5

AI scripts are Lua 5.5 source files placed in the pack's `ai/` directory. The engine calls
`compute_control(state, tick, dt)` each sim tick (60 Hz) and uses the returned table of
control inputs to drive the entity's flight integrator.

**File location:** `ai/<name>.lua` inside the content pack directory.  
**Function required:** `function compute_control(state, tick, dt) → table`

See [`docs/modding/ai.md`](../ai.md) for the complete API reference including the `state`
table fields, `guidance.*` math module, `nearby_entities()`, `get_entity()`, and worked
examples.

> **Lua 5.5 note:** `global` is a reserved keyword in Lua 5.5. Scripts that use `global`
> as a variable name will fail to load. Rename any such variables before shipping a pack
> targeting this engine version.
