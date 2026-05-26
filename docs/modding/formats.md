# Native Open Asset Format Specifications

These are the engine's canonical formats. All content packs are expected to provide assets in these formats.

For authoring tools and workflow guides, see the other files in this directory.

---

## 3D Models — glTF 2.0

- Aircraft, vehicles, weapons, buildings, terrain features
- Damage states: separate glTF meshes or morph targets (`_b` suffix = battle-damaged)
- LOD variants: glTF `LOD` extension or separate files (`F22_lod0.glb`, `F22_lod1.glb`)
- Animations: glTF `animations` array (gear extend/retract, prop rotation, bay doors)
- Shadow mesh: separate `F22_shadow.glb`
- Cockpit interior: optional separate `F22_cockpit.glb`; includes camera anchor point and
  instrument panel geometry. Instruments are non-interactive geometry (no DCS-style clickable cockpit).
- Toolchain: Blender → glTF 2.0 export (see `docs/modding/3d-models.md`)

---

## Textures — PNG + KTX2

- Source: PNG (RGBA, any resolution)
- GPU-ready: KTX2 with BC1/BC3/BC7 compression + mipmaps generated at pack time
- Naming: `aircraft_f22.png`, `terrain_grass.png` (lowercase, snake_case)
- Palette-mapped textures from FA are converted to full RGBA at import time

---

## Audio — OGG Vorbis / Opus

- Sound effects: OGG at 44.1 kHz stereo or mono
- Music: OGG (pre-rendered from MIDI/FluidSynth during content pack build)
- Content packs that use legacy audio formats handle their own conversion before providing OGG to the engine
- Streaming sources (long music tracks) use OpenAL streaming buffers

---

## Music Playlist — TOML

Controls which music track plays in each named game state.

```toml
# audio/playlist.toml
[crossfade]
duration_s = 3.0

[[states]]
id      = "menu"
tracks  = ["audio/music/menu_theme.ogg"]
loop    = true

[[states]]
id      = "flight_patrol"
tracks  = ["audio/music/patrol_01.ogg", "audio/music/patrol_02.ogg"]
loop    = true
shuffle = true

[[states]]
id      = "flight_combat"
tracks  = ["audio/music/combat_01.ogg", "audio/music/combat_02.ogg"]
loop    = true
shuffle = false

[[states]]
id      = "mission_success"
tracks  = ["audio/music/victory.ogg"]
loop    = false
```

State transitions are driven by engine events. Lua scripts can force a state change with `world.set_music_state("flight_combat")`.

---

## Flight Model — TOML

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

- Grid of fixed-size PNG chunks; any grid dimension (no 32×32 cap)
- Each chunk: 513×513 pixels, 16-bit grayscale
- Chunks loaded/unloaded at runtime based on player position

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

## AI Scripts — Lua 5.4

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
