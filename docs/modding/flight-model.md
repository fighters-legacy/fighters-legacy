# Flight Model Authoring Guide

This guide covers every field in the aircraft TOML flight model schema. It is aimed at mod
authors who want to populate accurate, physically meaningful values — from casual ("close enough
to fly well") through to hardcore ("match published NACA data for this specific airframe").

All TOML examples in this guide use SI units. Do not mix imperial and SI within a file.

---

## Referencing a flight model from an entity

An entity opts into a flight model by setting `flight_model` in its `[entity]` definition to the
flight-model asset id (the same lookup name passed to the asset loader):

```toml
[entity]
id           = "fl-base:f15c"
name         = "F-15C Eagle"
category     = "air_vehicle"
max_hp       = 100.0
mesh         = "aircraft/f15"
flight_model = "aircraft/f15_fm"   # resolves to this flight-model TOML
```

The server resolves `flight_model` to a parsed model when the entity is spawned; it is
**server-authoritative and never sent on the wire** (clients only render the state the server
broadcasts). If `flight_model` is omitted, empty, or the id cannot be resolved, the entity falls
back to the built-in UFO-like model (the same default used by the zero-content-pack sandbox).

---

## Physics model overview

The fighters-legacy flight model is a **simplified 6-DOF stability-derivative model**:

- The aircraft is a rigid body with 3 translational and 3 rotational degrees of freedom.
- Aerodynamic forces (lift, drag) and moments (pitch, roll, yaw) are expressed as sums of
  non-dimensional coefficients multiplied by dynamic pressure and a reference area or length.
- Coefficients are stored as tables indexed by angle of attack and Mach number, allowing stall,
  compressibility, and transonic effects to emerge naturally from the data.
- The integrator advances the state at 60 Hz using semi-implicit Euler, which is numerically
  stable at this timestep for the dynamics involved.

**Key emergent behaviours** (no special-case code):

| Behaviour | Source |
|---|---|
| Stall | CL table rolls off past critical AoA |
| G-effects | Net normal force / weight |
| Energy management | Drag polar + thrust tables |
| Transonic drag rise | `[aero.cd_wave]` table |
| Fuel consumption | Three-point throttle model |

**International Standard Atmosphere (ISA, ISO 2533)** is used to compute air density, speed of
sound, and dynamic pressure as a function of altitude. Key values:

| Altitude | Density (kg/m³) | Speed of sound (m/s) |
|---|---|---|
| 0 m (sea level) | 1.225 | 340.3 |
| 3 000 m | 0.909 | 328.6 |
| 6 000 m | 0.660 | 316.5 |
| 9 000 m | 0.467 | 304.1 |
| 11 000 m (tropopause) | 0.364 | 295.1 |
| 15 000 m | 0.194 | 295.1 |

---

## Coordinate system and sign conventions

All TOML derivative values follow **body-axis / stability-axis conventions** consistent with
NACA/NASA reporting standards. Authors can copy derivative values directly from technical reports
without sign reversal.

**Body-axis frame (right-hand rule).** The engine's body frame is **X forward, Y up, Z right**
(`AeroForces.cpp`, `IForceModel.h`) — a Y-up frame, matching the renderer and glTF. It is **not** the
NACA Z-down frame, and this document used to claim it was.

- X-axis: positive **forward** (out the nose)
- Y-axis: positive **up**
- Z-axis: positive **right** (starboard wing)

**What this means for you: nothing.** The derivative *sign conventions* below are the NACA ones, and
they are what you actually author — a negative `cm_alpha` is statically stable here exactly as it is
in every textbook and every DATCOM table, because the engine returns its moment vector as
`{roll, pitch, yaw}` rather than as components of a coordinate frame. **Transcribe published
derivatives with their published signs.** The frame is stated only so that nobody trying to reason
about the *forces* is misled.

**Aerodynamic angles:**

- **Alpha (α)**: angle of attack — positive when the nose is **above** the velocity vector
- **Beta (β)**: sideslip angle — positive when velocity comes **from the right** (nose-left yaw)

**Moments and angular rates:**

| Axis | Moment positive | Rate positive |
|---|---|---|
| Pitch | Nose up | Nose pitching up (q) |
| Roll | Right wing down | Rolling right (p) |
| Yaw | Nose right | Yawing right (r) |

**Control inputs** are normalised −1 to +1:

- Stick fore/aft (elevator): **+1 = pull = nose-up command**
- Stick left/right (aileron): **+1 = right roll command**
- Rudder pedals: **+1 = right yaw command**

**Consequence for derivative signs:**

- `cm_alpha < 0` → aircraft is statically stable in pitch (nose-up increases nose-down restoring moment)
- `cn_beta > 0` → aircraft is directionally stable (sideslip from the right causes rightward yaw, restoring alignment)

These match standard NACA sign conventions exactly.

---

## `[aircraft]` — Metadata, engine type, and role

```toml
[aircraft]
name         = "F/A-18C Hornet"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = true
cruise_alt_m = 12192
mesh         = "fa18c"
cockpit      = "fa18c_hud"
```

### `type` — Aircraft role

Controls AI tactics selection and UI filtering. Valid values:

| Value | AI behaviour |
|---|---|
| `"fighter"` | Dogfights, BVR, escort |
| `"interceptor"` | Climbs to high altitude, prioritises speed over turn rate |
| `"attacker"` | Stays low, prioritises ground targets |
| `"bomber"` | Maintains formation and heading, minimal manoeuvring |
| `"maritime_patrol"` | Orbits at medium altitude, monitors naval contacts |
| `"awacs"` | Orbits at high altitude, extends teammate radar |
| `"ew"` | Orbits or escorts, activates jamming |
| `"recon"` | Flies at max speed/altitude, avoids engagement |
| `"tanker"` | Follows assigned tanker track, dispenses fuel |
| `"transport"` | Follows waypoints, avoids combat |
| `"trainer"` | Same as fighter but no active weapons |

### `engine_type` — Propulsion category

Drives visual effects (jet exhaust vs. spinning props) and audio selection:
`"turbojet"` | `"turbofan"` | `"turboprop"` | `"piston"`

### `has_fbw` — Fly-by-wire

When `true`, the flight computer holds the aircraft inside its envelope even when the player has all
flight assists disabled — FBW aircraft cannot depart controlled flight in normal operation. The
limiter holds **angle of attack** (not measured g — elevator changes pitch rate, so a reactive
G-meter loop is always a tick late), and covers three limits (#816, #900):

- **positive structural g** — aft stick is held at the AoA that makes `max_g_structural`;
- **negative structural g** — forward stick is held against `min_g_structural`, just as firmly;
- **FLCS AoA cap** — the optional `alpha_limit_deg` (`[aero.limits]`) holds `|alpha|` below a limit
  the wing can aerodynamically exceed (the F-16 holds 25.5° while the wing stalls near 35°). At low
  dynamic pressure the wing cannot reach the structural g, and this cap is what holds the jet.

When `false` (F-15A, MiG-29 early variants), limits are purely player-toggleable assists — expert
players can fly raw and risk departure.

### `cruise_alt_m` — AI cruise altitude

The altitude at which AI wingmen, patrol aircraft, and bombers fly when no tactical altitude is
commanded. Set to the aircraft's most fuel-efficient cruise altitude (available in Jane's All the
World's Aircraft performance tables). Turboprops: typically 7 000–9 000 m. High-altitude jets:
10 000–15 000 m.

---

## `[flight_model]` — Mass, geometry, and inertia

```toml
[flight_model]
mass_kg      = 16651.0
wing_area_m2 = 46.5
wingspan_m   = 12.3
mac_m        = 3.51
fuel_kg      = 6531.0
ixx_kg_m2    = 14000.0
iyy_kg_m2    = 100000.0
izz_kg_m2    = 110000.0
```

| Field | Meaning | Source | Typical range (fighters) |
|---|---|---|---|
| `mass_kg` | Representative combat mass (structure + systems + payload, no fuel) | Jane's, Wikipedia | 8 000–25 000 kg |
| `wing_area_m2` | Reference wing planform area (used in all lift/drag calculations) | Jane's | 25–75 m² |
| `wingspan_m` | Tip-to-tip span; roll and yaw moment reference length | Jane's | 8–20 m |
| `mac_m` | Mean aerodynamic chord; pitch moment reference length. Typically 25–30% of root chord for swept-wing jets. | Jane's, DATCOM | 3–6 m |
| `fuel_kg` | Maximum internal fuel. Tracked live; reduces total mass in flight. | Jane's | 3 000–10 000 kg |
| `ixx_kg_m2` | Roll moment of inertia. Smaller → snappier rolls. | NACA/NASA TRs, DATCOM | 5 000–40 000 kg·m² |
| `iyy_kg_m2` | Pitch moment of inertia. Smaller → faster pitch response. | NACA/NASA TRs | 50 000–200 000 kg·m² |
| `izz_kg_m2` | Yaw moment of inertia. Usually ≥ Iyy. | NACA/NASA TRs | 60 000–220 000 kg·m² |
| `ixz_kg_m2` | *Optional (#899).* Roll↔yaw product of inertia; opts into the coupled solve. `Ixz² < Ixx·Izz`. | NACA/NASA TRs | 0 (default); F-16 ≈ 1 331 |
| `engine_ang_momentum` | *Optional (#899).* Engine rotor angular momentum He (N·m·s, signed) — gyroscopic pitch↔yaw coupling. | NASA TP-1538 | 0 (default); F-16 ≈ 216.9 |

**Sources for inertia data:** NACA/NASA Technical Reports Server (ntrs.nasa.gov). Search for
the aircraft type number or name alongside "stability derivatives" or "moments of inertia". USAF
Stability and Control DATCOM provides estimation methods when measured data is unavailable.

**Rule of thumb for Cold War jets:** Iyy ≈ Izz >> Ixx. Reducing Ixx by 30% makes the roll
feel noticeably snappier without affecting pitch or yaw response.

---

## `[aero.cl_table]` — Lift coefficient

```toml
[aero.cl_table]
alpha  = [-5, 0, 5, 10, 15, 18, 20, 25]   # degrees
mach   = [0.3, 0.6, 0.9, 1.2, 1.8]
values = [
    # Mach: 0.3   0.6   0.9   1.2   1.8
           -0.20,-0.22,-0.24,-0.18,-0.12,  # alpha = -5°
            0.05, 0.06, 0.07, 0.05, 0.03,  # alpha =  0°
            0.40, 0.45, 0.52, 0.40, 0.28,  # alpha =  5°
            0.75, 0.84, 0.97, 0.75, 0.52,  # alpha = 10°
            1.05, 1.18, 1.36, 1.05, 0.73,  # alpha = 15°
            1.18, 1.32, 1.52, 1.18, 0.82,  # alpha = 18° (near stall)
            1.10, 1.23, 1.42, 1.10, 0.76,  # alpha = 20° (post-stall)
            0.85, 0.95, 1.10, 0.85, 0.59,  # alpha = 25° (deep stall)
]
```

**What CL represents:** `CL = Lift / (q × S)` where q = ½ρV² is dynamic pressure and S is
`wing_area_m2`. The integrator computes `Lift = CL(α, M) × q × S`.

**Interpolation:** Bilinear over the (alpha, Mach) grid. Values outside the grid are clamped
to the nearest edge — no extrapolation.

**Choosing breakpoints:** Use denser spacing near stall (alpha 14–22°) where CL changes
rapidly. Coarser spacing at extremes is fine. Minimum: 4 alpha breakpoints, 2 Mach breakpoints.

**Stall is emergent:** CL peaks at `alpha_stall_deg` then falls off. No special-case stall
code fires. The aircraft departs controlled flight naturally as lift decreases and drag increases.

**Mach effect:** Below Mach 1, CL generally increases with Mach due to Prandtl-Glauert
compressibility (factor ≈ 1/√(1−M²)). Above Mach 1, CL drops. The table captures this
implicitly — authors enter the actual CL value at each condition.

**Sources:**

- NACA technical notes and reports (ntrs.nasa.gov) — search the aircraft designation alongside
  "lift coefficient" or "stability derivatives"
- **NASA TP-1538** — Nguyen et al., 1979, *Simulator Study of Stall/Post-Stall Characteristics of a
  Fighter Airplane With Relaxed Longitudinal Static Stability*. This is the **F-16**, and it is the
  only complete public-domain nonlinear aero database for any military jet — CL, CD and the moment
  derivatives tabulated against alpha and Mach. It is the single most useful document on this list.
- **NASA CR-2144** — Heffley & Jewell, 1972, *Aircraft Handling Qualities Data*. Dimensional
  derivatives for the **F-104A, F-4C, X-15, NT-33, XB-70A** and five others. It is **not** the F-16.
- NASA TM-86694 (F/A-18C) for a second modern-fighter cross-check.
- X-Plane Airfoil Tools for cross-checking subsonic 2D section data
- Jane's performance tables for cross-check: if stall speed V_stall and mass are known,
  CL_max = 2 × mass × g / (ρ × V_stall² × S)

---

## `[aero.drag_polar]` — Parasitic and induced drag

```toml
[aero.drag_polar]
cd0           = 0.016
k             = 0.12
speedbrake_cd = 0.08
gear_cd       = 0.03
```

Total drag: **CD = cd0 + k × CL² + cd_wave(Mach) + payload_drag + device_drag**

| Field | Meaning | Typical range (fighters) |
|---|---|---|
| `cd0` | Zero-lift (parasitic) drag in clean configuration | 0.012–0.025 |
| `k` | Induced drag factor; `k ≈ 1/(π × AR × e)` where `AR = wingspan²/wing_area` and `e ≈ 0.7–0.85` for swept-wing jets | 0.08–0.20 |
| `speedbrake_cd` | Additional CD when speedbrake/airbrake fully deployed | 0.04–0.12 |
| `gear_cd` | Additional CD with landing gear extended | 0.02–0.06 |

**Back-calculating cd0:** If the aircraft's published max speed at a given altitude is known,
and thrust at that condition is available from the thrust table:
`cd0 = Thrust / (q × S) − k × CL_trim²`
At high speed near max Mach, CL_trim ≈ 0, so `cd0 ≈ Thrust / (q × S)`.

**Tuning k for turn rate:** Increasing k adds induced drag at high AoA and reduces sustained
turn rate (corner velocity). Decreasing k allows higher sustained G at the cost of physical
accuracy.

**Speedbrake and gear drag** are applied as additive deltas, scaled by the actuator's **position**,
not by the command — a gear that takes six seconds to travel builds its drag over those six seconds.
See [`[articulation]`](#articulation--actuator-transit-times-optional). Both can be active
simultaneously and stack with wave drag and payload drag.

---

## `[aero.flaps]` — High-lift device (optional)

```toml
[aero.flaps]
dcl             = 0.60   # CL increment at full flap
dcd             = 0.055  # parasite-drag increment at full flap
alpha_shift_deg = 3.0    # shifts the CL-table lookup (optional)
```

| Field | Meaning |
|---|---|
| `dcl` | CL added at `flaps = 1`, scaling linearly with position |
| `dcd` | Parasite drag added at `flaps = 1`. Must be ≥ 0 — a flap cannot reduce parasite drag |
| `alpha_shift_deg` | Shifts where the CL table is sampled, so a given body AoA reads more CL **and the stall arrives at a lower body AoA** — which is what a real flap does |

Omitting the section means the aircraft models no flap: the flap switch still moves and still drives
the animation, but nothing changes aerodynamically. Every model written before this section existed
therefore flies exactly as it did.

---

## `[articulation]` — Actuator transit times (optional)

```toml
[articulation]
gear_transit_s       = 6.0
flap_transit_s       = 4.0
speedbrake_transit_s = 1.5
hook_transit_s       = 2.0
canopy_transit_s     = 5.0
```

Seconds for **full travel** (0 → 1) of each actuator. All fields are optional; the defaults above are
plausible for a light fighter, so an existing model keeps parsing and keeps flying.

**Transit timing lives here, never in the animation clip.** The renderer *scrubs* a clip to the
actuator's current position (see [`docs/modding/3d-models.md`](3d-models.md#animation-channels)), so
retiming an animation changes how the gear looks, not how long it takes. One number drives the
server's physics, the client's prediction replay and the visual.

Reversing mid-travel reverses from the current position — there is no snap. `0` means instantaneous,
which is the honest reading of a model that declares no travel time for an actuator; the validator
rejects negative values and anything over 600 s, and warns below 0.1 s.

---

## `[aero.cd_table]` — Tabulated drag (optional; replaces the parabolic polar)

A `Table2D(alpha_deg, mach) -> CD`, structurally identical to `[aero.cl_table]`.

**Why it exists.** A parabolic polar (`cd0 + k·CL²`) forces the implied induced-drag coefficient to be
*constant* across a specific-excess-power chart. Real fighters do not behave that way — against the
F-5E's flight manual the implied coefficient grows **3.5×** from the 1–2 g region to the 4–5 g region,
because a real wing's drag rises far faster than CL² near max lift. Fit `k` to cruise and the aircraft
out-turns the real thing by 18%; fit it to the hard-turn end and cruise drag is overstated, wrecking
range and acceleration. **No single value of `k` gives both.** It is also the form real data arrives
in: NASA TP-1538 publishes the F-16's CD as a table against alpha and Mach, which cannot be
transcribed into a polar at all.

```toml
[aero.cd_table]
alpha  = [-10, -5, 0, 5, 10, 15, 20, 25]   # >= 4 breakpoints
mach   = [0.2, 0.6, 0.9, 1.2, 1.6]         # >= 2 breakpoints
values = [ ... ]                            # row-major, len(alpha) x len(mach); all > 0
```

**When present it REPLACES `cd0 + k·CL²` entirely** — the table is *total clean drag* and already
includes induced drag. `cd_wave`, `speedbrake_cd`, `gear_cd` and the payload's drag still add on top.

**Set `drag_polar.k = 0.0` when you use it.** Authoring both a `cd_table` and a non-zero `k` is a
validator **error**, because the induced term would be counted twice — a silent 2× drag bug you would
have no way to debug from the outside.

If you have no tabulated data, ignore this block: `[aero.drag_polar]` remains the simple path, and it
is what most content will use.

## `[aero.cd_wave]` — Transonic wave drag

```toml
[aero.cd_wave]
mach   = [0.7, 0.8, 0.85, 0.9, 0.95, 1.0, 1.05, 1.1, 1.2, 1.5]
values = [0.000, 0.002, 0.010, 0.028, 0.042, 0.038, 0.028, 0.018, 0.008, 0.003]
```

Added to CD_total as `cd_wave(Mach)`. This block is **optional** — omit for aircraft that
never approach transonic (subsonic trainers, most turboprops).

**Why it matters:** Without this block, an afterburning jet can push through Mach 1 with no
extra drag cost. With it, the drag rise around the sound barrier is physical and creates the
"wall" that energy management around Mach 1 is about.

**Shape:** Near-zero below Mach 0.7. Rises sharply from ~Mach 0.85 to peak at ~Mach 1.0
(typical peak ΔCD: 0.03–0.06 for fighters). Falls back to near-zero above Mach 1.2.

Interpolated linearly between breakpoints. Zero outside the specified Mach range.

**Sources:** NACA RM (Research Memorandum) series on transonic aerodynamics. NASA TM for
specific aircraft. Published CD-vs-Mach drag polars in Jane's or GlobalSecurity.

**Tuning tip:** If the aircraft "sticks" at Mach 0.99 but breaks through instantly, reduce the
peak value or shift the peak breakpoint slightly above Mach 1.0.

---

## `[aero.moments]` — Stability derivatives

```toml
[aero.moments]
cm_alpha = -0.8
cm_q     = -12.0
cm_de    = -1.2
cl_beta  = -0.09
cl_p     = -0.45
cl_da    =  0.08
cn_beta  =  0.12
cn_r     = -0.15
cn_dr    = -0.06
```

All derivatives are non-dimensional and follow NACA sign conventions (see coordinate system
section above). Values can be copied directly from NACA/NASA technical reports.

The moment applied by each derivative (optional #899 terms shown in brackets — all default 0):
- **Pitch moment** (about Y-axis, reference length mac_m):
  `Cm = [cm0] + cm_alpha×α + cm_q×(q_rate×mac/(2V)) + cm_de×(elevator×max_elevator_rad) [+ cm_speedbrake×speedbrake]`
- **Roll moment** (about X-axis, reference length wingspan_m):
  `Cl = cl_beta×β + cl_p×(p_rate×span/(2V)) + cl_da×(aileron×max_aileron_rad) [+ cl_dr×(rudder×max_rudder_rad)]`
- **Yaw moment** (about Z-axis, reference length wingspan_m):
  `Cn = cn_beta×β + cn_r×(r_rate×span/(2V)) + cn_dr×(rudder×max_rudder_rad) [+ cn_da×(aileron×max_aileron_rad)]`

**Note:** Rate terms divide by velocity V. The integrator guards V < 1 m/s and sets rate
moment contributions to zero at near-zero speed.

### Pitch derivatives

| Derivative | Sign | Range (fighters) | Source |
|---|---|---|---|
| `cm_alpha` | Negative = statically stable | −0.3 to −1.5 | NACA TR 711, NASA TP-1538 (F-16) |
| `cm_q` | Always negative | −5 to −25 | NACA TN 2283 |
| `cm_de` | Negative (trailing-edge-down = nose-up moment) | −0.5 to −2.5 | DATCOM, NACA reports |

More negative `cm_alpha` → stronger nose-down tendency at high AoA (more stable, less agile).
Larger magnitude `cm_q` → pitch oscillations damp out faster.

### Roll derivatives

| Derivative | Sign | Range (fighters) | Source |
|---|---|---|---|
| `cl_beta` | Negative = stable dihedral | −0.05 to −0.25 | NACA TN 1285 |
| `cl_p` | Always negative | −0.2 to −0.6 | NACA TR 868 |
| `cl_da` | Positive | 0.04 to 0.15 | DATCOM |

More negative `cl_p` → roll rate decays faster when stick is centred.
Larger `cl_da` → snappier roll response.

### Yaw derivatives

| Derivative | Sign | Range (fighters) | Source |
|---|---|---|---|
| `cn_beta` | Positive = weathercock stable | 0.05 to 0.25 | NASA TP-1538 (F-16) |
| `cn_r` | Always negative | −0.05 to −0.25 | NACA TN 2235 |
| `cn_dr` | Negative | −0.03 to −0.12 | DATCOM |

### Optional advanced terms (#899)

Published aerodynamic databases (e.g. NASA TP-1538 for the F-16) carry several quantities the base
schema has no slot for. All are **optional and additive** — a model that omits them is byte-identical
to before. Add them to `[aero.moments]` (or `[flight_model]` / `[aero.drag_polar]` where noted).

| Field | Section | Meaning | Default |
|---|---|---|---|
| `cm0` | `[aero.moments]` | Zero-α pitching moment. A cambered wing trims at a non-zero α with neutral elevator (≈ −3.3° for the F-16A); `cm0 ≠ 0` is what produces that offset. | 0 |
| `cn_da` | `[aero.moments]` | Adverse yaw — aileron deflection yaws *against* the commanded roll. | 0 |
| `cl_dr` | `[aero.moments]` | Rudder-induced roll — rudder deflection also rolls the aircraft. | 0 |
| `cm_speedbrake` | `[aero.moments]` | Pitch increment per unit speed-brake deployment (ΔCm,sb). | 0 |
| `speedbrake_cl` | `[aero.drag_polar]` | Speed-brake lift/normal-force increment per unit deployment (ΔCZ,sb) — an airbrake changes lift as well as drag. | 0 |
| `ixz_kg_m2` | `[flight_model]` | Product of inertia between the roll (x) and yaw (z) axes. A non-zero value opts the airframe into the Ixz-coupled roll/yaw solve — a roll moment then produces a yaw acceleration and vice versa (F-16 = 1,331). Must satisfy `Ixz² < Ixx·Izz`. | 0 |
| `engine_ang_momentum` | `[flight_model]` | Engine rotor angular momentum He (N·m·s) about +X. A spinning spool is a gyroscope: pitching yaws it and yawing pitches it. Signed by spin direction (F-16 = 216.9). | 0 |

**Alpha-dependent dynamic dampers.** `cm_q`, `cl_p` and `cn_r` may each be supplied as a `Table1D`
over α (`cm_q_table`, `cl_p_table`, `cn_r_table`) — TP-1538 publishes them that way (Cmq runs −3.4 to
−6.8 across the sweep), which matters for a high-α or deep-stall airframe. When present, the table
**replaces** the corresponding scalar; the scalar stays required as the fallback.

```toml
[aero.moments.cm_q_table]   # optional — replaces the cm_q scalar; lookup over alpha (deg)
alpha  = [0.0, 15.0, 25.0, 35.0]
values = [-3.4, -5.0, -6.4, -6.8]
```

**A note on Ixz scope.** Declaring `ixz_kg_m2` adds only the Ixz İ·ω̇ cross-coupling; the general
`ω×H` rate-product Euler terms (which are non-zero even at Ixz = 0) remain omitted so existing content
is untouched. The cross-coupling is the term that matters most for a real fighter's roll/yaw handling.

### Tuning guide

| Symptom | Fix |
|---|---|
| Sluggish roll response | Increase `cl_da` or decrease `|cl_p|` |
| Pitch oscillation (PIO) | Decrease `|cm_de|` or increase `|cm_q|` |
| Aircraft wanders in yaw | Increase `cn_beta` |
| Falling leaf / excessive roll-yaw coupling | Reduce `|cl_beta|` or increase `|cn_beta|` |
| Nose drops too hard at stall | Make `cm_alpha` less negative |

---

## `[aero.limits]` — Structural envelope

```toml
[aero.limits]
alpha_stall_deg  =  18.0
max_g_structural =   9.0
min_g_structural =  -3.0
max_mach         =   1.8
alpha_limit_deg  =  25.5   # optional (#900): FBW FLCS AoA cap, below the aero stall
max_keas         = 710.0   # optional (#1181): dynamic-pressure placard, knots EAS
```

| Field | Meaning |
|---|---|
| `alpha_stall_deg` | The AoA at which this aircraft departs. **`validate-flight-model` requires your `cl_table` to peak within 2° of it** — the engine does not clamp CL at the stall, because your table *is* the stall; if the two disagree, the model is lying about itself. Sets `FlightState::stalled`, which drives buffet, the HUD cue and AI. |
| `max_g_structural` | Positive structural limit. Exceeding it by >10% for >0.5 s **damages the airframe**. Range: 6.5 g (heavy strikers) to 9 g (dogfighters). Source: flight manual / Jane's. |
| `min_g_structural` | Negative structural limit. Typical −2.5 g to −3.5 g. Same damage rule; on an FBW aircraft the limiter now holds forward stick off it (#900). |
| `max_mach` | Never-exceed Mach. **Not** enforced by an artificial drag wall: an aircraft's top speed comes from drag rising to meet thrust. `fm-trim` fails a model that can exceed this in level flight — if it can, the *model* is wrong, and you should fix `cd_wave` or the thrust deck. |
| `alpha_limit_deg` | *Optional (#900).* The FBW flight computer's AoA cap, **distinct from `alpha_stall_deg`** (the aero peak). When set (>0) and `has_fbw`, the limiter also holds `|alpha|` below this — the F-16's FLCS holds 25.5° while its wing stalls near 35°. Must sit below `alpha_stall_deg` or it never binds (the validator warns). 0 / omitted = structural-g limiting only, as before. |
| `max_keas` | *Optional (#1181).* The airframe's **dynamic-pressure placard**, in knots equivalent airspeed. Flight manuals state a top speed as a pair — "710 KEAS **or** Mach 1.3, whichever is less" — and this is the half `max_mach` cannot express. `fm-trim` caps max level speed at whichever binds first, so one number covers every altitude: EAS is the speed carrying sea-level dynamic pressure, so the placard bites hard down low and not at all in the stratosphere. 0 / omitted = Mach-limited only, as before. |

### When you need `max_keas`

If your aircraft has thrust to spare at low altitude, `max_mach` alone will let it fly far too fast
down low, and **no `cd_wave` shape can fix it**. The B-1B is the worked case: it is placarded at
608 kn at 200–500 ft while carrying roughly 655 kN of installed thrust against roughly 276 kN of
drag there, so nothing aerodynamic holds it — the model trims to ~M1.02 against a published M0.92,
about 11% fast in exactly the low-level regime the aircraft exists for.

Shaping `cd_wave` steeply enough to stop it at sea level does not work either: the same curve then
prevents the aircraft accelerating through M1.0 at 50,000 ft, where dynamic pressure is 4.7× lower,
and its *other* published number — max Mach at altitude — becomes unreachable. One Mach-dependent
drag term cannot serve both a sea-level q limit and a stratospheric thrust limit. That is what this
field is for.

**On FBW aircraft** (`has_fbw = true`) the flight computer keeps the pilot inside the envelope by
limiting AoA to whatever produces the applicable limit at the current dynamic pressure: aft stick
against `max_g_structural`, forward stick against `min_g_structural` (#900), and — if set — `|alpha|`
against `alpha_limit_deg`. **On everything else there is no limiter at all** — and that is deliberate.
An F-5E pilot *can* overstress the jet, and the sim lets them, and then bills them. `has_fbw` gates
the limiter and nothing else.

---

## `[aero.controls]` — Maximum control surface travel

```toml
[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 30.0
```

**Why this section exists:** The moment derivatives `cm_de`, `cl_da`, `cn_dr` are expressed
per radian of actual surface deflection (as published in NACA/DATCOM reports). The integrator
maps normalised stick input (−1 to +1) to surface deflection using these fields:

```
elevator_rad = player_input × max_elevator_deg × π/180
```

Without this section, authors would have to pre-multiply their NACA values by an assumed max
travel — an invisible assumption that produces wrong handling if guessed incorrectly.

| Field | Meaning | Typical range |
|---|---|---|
| `max_elevator_deg` | Nose-UP travel at full aft stick | 20–30° (stabilator), 15–25° (elevator) |
| `max_elevator_neg_deg` | *Optional.* Nose-DOWN travel at full forward stick. Absent ⇒ symmetric (= `max_elevator_deg`) | F-5E: 5° against 17° nose-up |
| `max_aileron_deg` | ±max aileron trailing-edge-down deflection | 15–25° |
| `max_rudder_deg` | ±max rudder deflection | 25–35° |

**Pitch travel is usually asymmetric, and the schema now says so (#822).** A fighter needs far more
nose-up authority than nose-down: the F-5E's all-moving stabilator travels **17° up but only 5° down**
(T.O. 1F-5E-1), a 3.4:1 ratio, and the F-16's and T-38's are asymmetric too. With one number you must
choose — and choosing the nose-up figure (the one that governs pitch authority and reaching the G
limit) models your aircraft's nose-down authority **3.4× too generous**, so a bunt or a negative-G
push is far more effective than the real aeroplane's. Author both, and it is right.

`max_elevator_neg_deg` is a **travel magnitude**: it is positive, and the sign is carried by the
stick. The validator rejects a negative value, and warns if nose-down travel exceeds nose-up (possible
on a canard; almost always a transcription slip on a conventional fighter).

**Roll and yaw deliberately have no negative-side key.** Left and right are mirror images, so a
sign-dependent travel would mean an aircraft that rolls harder one way than the other. An aileron
quoted as "35° up / 25° down" is a per-*surface* differential, not a per-command asymmetry; and a
gear-driven aileron spring stop (as on the F-5E) is a control-system behaviour — a soft stop the pilot
can overpower at the cost of a structural limit — not a travel limit. Author the effective value.

Sources: aircraft flight manuals, FAA/military type certificates, Jane's systems descriptions,
DATCOM (which reports control authority in degrees).

---

## `[wing_sweep]` — Variable wing sweep (optional)

Present only on variable-geometry aircraft (F-14 Tomcat, F-111, Su-17, MiG-23). Omit for all
fixed-geometry aircraft.

```toml
[wing_sweep]
ref_sweep_deg   = 55.0
min_deg         = 20.0
max_deg         = 68.0
slew_rate_deg_s =  7.5

[wing_sweep.schedule]
mach  = [0.0, 0.4, 0.7, 0.9, 1.2, 1.8]
sweep = [20,   20,  50,  60,  67,  68]

[wing_sweep.spread]
cl_scale  =  1.20
k_scale   =  0.80
cd0_delta = +0.004

[wing_sweep.swept]
cl_scale  =  0.82
k_scale   =  1.30
cd0_delta = -0.003
```

**How it works:** The base `[aero.cl_table]` and `[aero.drag_polar]` are measured at
`ref_sweep_deg`. At any current sweep angle θ, the integrator computes:

```
t = (θ − min_deg) / (max_deg − min_deg)   # 0 at spread, 1 at swept
cl_scale  = lerp(spread.cl_scale,  swept.cl_scale,  t)
k_scale   = lerp(spread.k_scale,   swept.k_scale,   t)
cd0_delta = lerp(spread.cd0_delta, swept.cd0_delta, t)
```

Applied: `CL_eff = CL_table × cl_scale`, etc.

**Authoring:**

- Set `ref_sweep_deg` to the sweep angle at which your CL table data was measured. For
  NACA wind-tunnel data on the F-14, most data was taken at ~55° mid-sweep.
- Source `spread.cl_scale` from the ratio of published CL_max at `min_deg` vs. at
  `ref_sweep_deg` (Jane's, NASA TM-81168 F-14 data).
- `k_scale ≈ AR_ref / AR_spread` for the spread config (spread gives ~40% more effective AR
  than 55° for the F-14).
- `slew_rate_deg_s` from aircraft specifications (F-14: ~7.5°/s).
- `[wing_sweep.schedule]` is the Mach-driven auto programme (Mach is true airspeed —
  aircraft velocity relative to the air mass, not ground speed). Pilots and AI can override it
  with a manual sweep command.

**Parser validates** that `min_deg ≤ ref_sweep_deg ≤ max_deg`.

---

## `[aero.tvc]` — Thrust vector control (optional)

Present only on TVC-capable aircraft (F-22, Su-37). Omit for all others.

```toml
[aero.tvc]
min_angle_deg   = -20
max_angle_deg   =  20
slew_rate_deg_s =   5
```

`min_angle_deg` / `max_angle_deg`: nozzle deflection range from the thrust axis. Typically ±20°
for production TVC. `slew_rate_deg_s`: how fast the nozzle moves; affects transient post-stall
manoeuvrability. The TVC moment is computed as `thrust × sin(nozzle_angle)` applied about the
appropriate axis.

---

## `[prop]` — Propeller torque (optional, omit for jet aircraft)

Present only for prop-driven aircraft. Omit for all jets.

```toml
[prop]
rotation      = "cw"
torque_factor = 0.06
gyro_factor   = 0.03
```

| Field | Values / meaning |
|---|---|
| `rotation` | `"cw"` — clockwise viewed from behind (most US/NATO single-props); `"ccw"` — counter-clockwise (some Soviet designs); `"contra"` — contra-rotating (Tu-95, Tu-114; net torque ≈ 0) |
| `torque_factor` | Roll moment = `torque_factor × thrust_N`. Applied opposite to prop rotation. Range: 0.03–0.12 for single-engine props. Set to 0.0 for contra-rotating. |
| `gyro_factor` | Gyroscopic precession — pitching up creates a yaw input. Range: 0.01–0.06. Set to 0.0 for contra-rotating. |

**Tu-95 example (contra-rotating, net torque ≈ zero):**

```toml
[prop]
rotation      = "contra"
torque_factor = 0.0
gyro_factor   = 0.0
```

**Thrust table shape for props:** Unlike jets (which gain thrust with ram compression), prop
thrust falls with airspeed. Enter high static thrust at Mach 0.0 that decreases through the
Mach breakpoints. The `[engine.mil_thrust]` table captures this naturally — no special handling
required. Max Mach for turboprops (Tu-95): ~0.82.

Sources: Jane's; NACA propeller efficiency charts; convert shaft horsepower to static thrust via
`T_static ≈ P × η / V` (with a separate static-thrust measurement for V = 0 since this formula
diverges).

---

## `[engine]` — Thrust tables and fuel model

```toml
[engine]
fuel_flow_idle_kg_s = 0.15
fuel_flow_mil_kg_s  = 1.20
fuel_flow_ab_kg_s   = 2.50
spool_time_s        = 5.0

[engine.mil_thrust]
mach   = [0.0, 0.3, 0.6, 0.9, 1.2, 1.5, 1.8]
alt_km = [0, 3, 6, 9, 12, 15]
values = [
    # alt: 0km  3km   6km   9km  12km  15km
          80.0, 68.0, 56.0, 44.0, 32.0, 20.0,  # Mach 0.0
          84.0, 72.0, 59.0, 47.0, 34.0, 21.0,  # Mach 0.3
          88.0, 75.0, 62.0, 49.0, 36.0, 23.0,  # Mach 0.6
          91.0, 78.0, 64.0, 51.0, 37.0, 24.0,  # Mach 0.9
          88.0, 75.0, 62.0, 49.0, 35.0, 22.0,  # Mach 1.2
          82.0, 70.0, 58.0, 46.0, 33.0, 21.0,  # Mach 1.5
          75.0, 64.0, 53.0, 42.0, 30.0, 19.0,  # Mach 1.8
]   # values in kN

[engine.ab_thrust]  # optional — omit for non-afterburning aircraft
mach   = [0.0, 0.3, 0.6, 0.9, 1.2, 1.5, 1.8]
alt_km = [0, 3, 6, 9, 12, 15]
values = [
          130.0, 112.0, 94.0, 76.0, 55.0, 34.0,
          137.0, 118.0, 99.0, 80.0, 58.0, 36.0,
          143.0, 123.0, 104.0, 84.0, 61.0, 38.0,
          148.0, 127.0, 107.0, 87.0, 63.0, 39.0,
          143.0, 123.0, 103.0, 84.0, 61.0, 38.0,
          134.0, 115.0, 96.0, 78.0, 57.0, 35.0,
          122.0, 105.0, 88.0, 71.0, 52.0, 32.0,
]   # values in kN
```

**Scalar fields must precede subtable headers in TOML.** Placing `fuel_flow_*` or `spool_time_s`
after `[engine.mil_thrust]` puts them at the document root, not inside `[engine]`. The parser
will reject this structure.

### Thrust tables

Bilinear interpolation over (Mach, alt_km). Clamped at edges. Values in kN.

**Suggested breakpoints:**

| Mach | Captures |
|---|---|
| 0.0 | Sea-level static (take-off, hover for test) |
| 0.3 | Low subsonic cruise |
| 0.6 | Mid-subsonic |
| 0.9 | High subsonic / transonic entry |
| 1.2 | Low supersonic |
| 1.5 | Mid-supersonic |
| 1.8+ | High-speed limit |

| alt_km | Captures |
|---|---|
| 0 | Sea level |
| 3 | Low altitude |
| 6 | Medium altitude |
| 9 | High altitude |
| 12 | Cruise ceiling (most jets) |
| 15 | Service ceiling |

**Sources:** Published thrust curves (Jane's, GlobalSecurity.org), NASA engine test reports.
Back-calculate from max speed: at V_max and known altitude, drag = thrust → solve for thrust at
that Mach/altitude cell.

**Tuning:** If the aircraft can't reach its published max speed, increase the thrust value at
the corresponding Mach/altitude cell. If it accelerates too fast, reduce it.

### Idle-thrust deck (optional)

By default, thrust below MIL is a straight line: `throttle × mil`, i.e. `0 → mil` as the throttle
moves `0 → 1`. That treats idle as *zero* thrust, which is wrong. Real turbofan idle thrust is
non-zero and non-linear — a small positive figure static, but at altitude and speed it goes
**negative**, because ram drag through the engine exceeds the idle gross thrust.

Add an optional `[engine.idle_thrust]` table — the **same `(mach, alt_km)` grid** and **kN units**
as `mil_thrust` — and the engine blends `idle → mil` across throttle `[0, 1]` instead of `0 → mil`.
Absent, behaviour is unchanged (the straight line stays the default, which is fine for most
content). Values may be negative; a deck published in newtons is divided by 1000 on authoring.

```toml
[engine.idle_thrust]   # optional — model part-throttle idle deck (descents, approach)
mach   = [0.0, 0.9]
alt_km = [0, 12]
values = [
           2.8,   1.0,    # M0.0: +2.8 kN static SL, +1.0 kN at 12 km
         -16.0, -10.0,    # M0.9: net drag — ram drag exceeds idle gross thrust
]   # values in kN
```

**Source:** engine idle-thrust decks are published alongside MIL/max in NASA reports (e.g. TP-1538
Table VI gives the F-16's `T_idle` from +2,824 N static to −16,013 N at M 1.0). Only the AB branch
ignores the deck; when the afterburner is lit, `[engine.ab_thrust]` is used outright.

### Fuel burn model

Piecewise linear: throttle 0 → 1 maps `fuel_flow_idle_kg_s` → `fuel_flow_mil_kg_s`. When AB
is engaged, flow jumps to `fuel_flow_ab_kg_s`.

| Field | Meaning | Typical range |
|---|---|---|
| `fuel_flow_idle_kg_s` | Ground idle burn (minimum). | 3–5% of MIL flow |
| `fuel_flow_mil_kg_s` | MIL (100% throttle, no AB). | 1–3 kg/s for turbofans |
| `fuel_flow_ab_kg_s` | Full afterburner. Ignored if `[engine.ab_thrust]` absent. | 3–5× MIL |

Convert from Jane's data: `kg/s = lb_hr ÷ 7936`, `kg/s = lb_min × 0.454 / 60`

### Engine spool

`spool_time_s`: first-order lag time constant. Throttle_actual advances toward commanded via:
`actual += (commanded − actual) / spool_time_s × dt`

Typical turbofan: 3–8 s idle→MIL. Set to 0 for instantaneous. The spool delay makes energy
management consequential: pushing AB in a merge doesn't deliver power immediately.

---

## `[carrier]` — Carrier operations (optional block)

> **Parsed but not yet consumed.** The engine reads and stores this block; nothing acts on it
> yet. Author it if you like — it will start working without a content change — but do not spend a
> day tuning numbers that currently do nothing.

Block presence indicates carrier-capable. Omit entirely for land-based aircraft.

```toml
[carrier]
approach_m_s     = 69.4
approach_aoa_deg =  8.1
cat_min_m_s      = 66.9
hook_length_m    =  5.33
```

| Field | Meaning | Source |
|---|---|---|
| `approach_m_s` | On-speed approach airspeed. Convert from knots: `m/s = kts × 0.5144` | NATOPS, Jane's |
| `approach_aoa_deg` | On-speed AoA — pilot and auto-throttle maintain this in the groove. Used by FLOLS indicator and carrier auto-throttle. | NATOPS, NASA/NACA approach data |
| `cat_min_m_s` | Minimum safe airspeed at end of catapult stroke. | Jane's |
| `hook_length_m` | Tailhook-to-main-gear axle distance. Affects wire engagement geometry. Convert from feet: `m = ft × 0.3048` | Aircraft spec sheets |

**Cross-check:** Verify that the CL table at `approach_aoa_deg` and approach Mach produces the
required CL within ~5%:

```
approach_mass_kg  = mass_kg + 0.3 × fuel_kg   # typical landing weight
approach_mach     = approach_m_s / 340.3       # at sea level
CL_required       = 2 × approach_mass_kg × 9.81 / (1.225 × approach_m_s² × wing_area_m2)
```

Compare with `CL(approach_aoa_deg, approach_mach)` from the table.

---

## `[refueling]` — In-flight refueling reception (optional block)

> **Parsed but not yet consumed.** The engine reads and stores this block; nothing acts on it
> yet. Author it if you like — it will start working without a content change — but do not spend a
> day tuning numbers that currently do nothing.

Block presence indicates the aircraft can receive fuel. Omit if not capable.

```toml
[refueling]
type          = "boom"
max_rate_kg_s = 4.5
```

| Value | Meaning |
|---|---|
| `"boom"` | USAF-style rigid boom received in a boom receptacle |
| `"drogue"` | Probe-and-drogue (Navy and most non-US aircraft) |

`max_rate_kg_s`: maximum intake rate. Game uses `min(tanker.max_rate_kg_s, this.max_rate_kg_s)`.
Convert: `kg/s = lb_min × 0.454 / 60`.

**Compatibility:** `refueling.type` must match the tanker's `tanker.type` (or the tanker must
have `type = "both"`). The engine checks compatibility before allowing contact.

---

## `[tanker]` — Fuel dispensing (optional block)

> **Parsed but not yet consumed.** As above.

Block presence indicates the aircraft can provide fuel. Omit for non-tanker aircraft. An
aircraft can have both `[refueling]` (it can receive) and `[tanker]` (it can dispense) — for
example, the Il-78 and some configurations of the S-3 Viking.

```toml
[tanker]
type            = "boom"
stations        = 1
max_rate_kg_s   = 4.5
offload_reserve = 0.20
```

| Field | Meaning |
|---|---|
| `type` | `"boom"` / `"drogue"` / `"both"` (KC-10, Il-78 with both boom and wing drogues) |
| `stations` | Simultaneous receivers. Most tankers: 1. KC-10 / Il-78: up to 3. |
| `max_rate_kg_s` | Max dispensing rate per station. |
| `offload_reserve` | Fraction of `fuel_kg` the tanker keeps for its own return. Transferable fuel = `current_fuel − reserve × fuel_kg`. AI tankers RTB when reserve is reached. |

---

## Payload interaction (no schema changes needed)

> **Hardpoints are not in the flight model.** `[[hardpoints]]` moved to the **entity definition
> TOML** in #623 — what an airframe is *allowed to carry* is a property of the entity, not of its
> aerodynamics. See [`formats.md`](formats.md#hardpoints-optional--weapon-stations). A flight model
> that still declares `[[hardpoints]]` is a validation error, with a message pointing there. The
> physical consequence of a loadout still reaches the flight model — through the mass and drag below,
> which is the only coupling that should exist.

The `drag_factor` and `weight_lb` fields in each weapon TOML are automatically consumed by the
flight integrator on each tick:

```
effective_cd0  = cd0 + sum(weapon.drag_factor)
effective_mass = mass_kg + current_fuel_kg + sum(weapon.weight_kg)
```

A fully loaded strike aircraft is measurably heavier and draggier than a clean aircraft. The
weapon TOML values (e.g., AIM-120C: `drag_factor = 0.008`, `weight_lb = 335`) should be
realistic so aircraft feel correctly penalised for carrying heavy ordnance.

---

## Flight assists — what schema fields they consume

| Assist | Fields consumed | Status |
|---|---|---|
| G-limiter (FBW only) | `max_g_structural`, `has_fbw` | **Implemented** — limits AoA to hold the structural limit |
| Over-G damage (all aircraft) | `max_g_structural`, `min_g_structural` | **Implemented** |
| Stall flag + buffet | `alpha_stall_deg` | **Implemented** |
| Auto-leveling | `cm_q`, `cl_p` (via moment derivatives — no extra fields) |
| Auto-throttle | `[engine]` tables, `fuel_flow_*`, `spool_time_s` |
| Carrier auto-throttle | `approach_m_s`, `approach_aoa_deg` |
| Simplified landing | `approach_m_s` (carrier) or 1.3× stall speed (land-based) |
| AI cruise | `cruise_alt_m`, `[engine]` tables |

On FBW aircraft (`has_fbw = true`), G-limiter and AoA limiter are always active regardless of
the player's assist settings.

---

## Known limitations

These simplifications are intentional for Phase 2:

- **Diagonal inertia tensor**: The cross-coupling term Ixz is assumed zero. Minor inaccuracy in
  Dutch roll / spiral mode for high-sweep aircraft. A future `ixz_kg_m2` field can be added
  without breaking existing TOML.
- **Variable sweep accuracy**: The multiplier approach captures essential gameplay feel but does
  not model non-linear aerodynamic effects at intermediate angles precisely. A future
  per-sweep-angle CL table is the natural extension.
- **Scalar moment derivatives**: No Mach variation. Second-order effect at this fidelity tier.
- **Side force neglected**: Lateral aerodynamic force from sideslip is not modelled as a
  separate force. Sideslip effects enter only through moment derivatives.
- **No flap modelling**: Flap CL/CD effects not in schema. Simplified landing assist compensates
  at game-logic level.
- **Ground effect not modelled**: Lift increase and induced drag decrease near the ground not
  implemented.
- **Helicopters out of scope**: Rotorcraft require a completely different FDM (rotor disc
  theory). No rotorcraft support in this schema.
- **No per-engine failure**: Multi-engine aircraft are modelled as a single combined engine.
  Asymmetric thrust from engine failure is not simulated in Phase 2.

---

## Quick-start template (generic fighter)

Use this as a starting point for any modern swept-wing combat jet. Replace the `[aircraft]`
metadata and tune `mass_kg`, the thrust table magnitudes, and `fuel_kg` for your specific
aircraft. The aerodynamic shape and derivatives produce flyable but generic behaviour without
additional research.

```toml
[aircraft]
name         = "Generic Fighter"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 10000
mesh         = "your_mesh_here"
cockpit      = "your_hud_here"

[flight_model]
mass_kg      = 12000.0
wing_area_m2 = 35.0
wingspan_m   = 10.0
mac_m        = 3.5
fuel_kg      = 4000.0
ixx_kg_m2    = 10000.0
iyy_kg_m2    = 70000.0
izz_kg_m2    = 78000.0

[aero.cl_table]
alpha  = [-5, 0, 5, 10, 15, 18, 20, 25]
mach   = [0.3, 0.6, 0.9, 1.2, 1.8]
values = [
    -0.20,-0.22,-0.24,-0.18,-0.12,
     0.05, 0.06, 0.07, 0.05, 0.03,
     0.40, 0.45, 0.52, 0.40, 0.28,
     0.75, 0.84, 0.97, 0.75, 0.52,
     1.05, 1.18, 1.36, 1.05, 0.73,
     1.18, 1.32, 1.52, 1.18, 0.82,
     1.10, 1.23, 1.42, 1.10, 0.76,
     0.85, 0.95, 1.10, 0.85, 0.59,
]

[aero.drag_polar]
cd0           = 0.018
k             = 0.14
speedbrake_cd = 0.07
gear_cd       = 0.03

[aero.cd_wave]
mach   = [0.75, 0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.20]
values = [0.000, 0.008, 0.022, 0.038, 0.034, 0.024, 0.014, 0.005]

[aero.moments]
cm_alpha = -0.7
cm_q     = -10.0
cm_de    = -1.0
cl_beta  = -0.08
cl_p     = -0.40
cl_da    =  0.07
cn_beta  =  0.10
cn_r     = -0.12
cn_dr    = -0.05

[aero.limits]
alpha_stall_deg  =  18.0
max_g_structural =   8.0
min_g_structural =  -3.0
max_mach         =   1.6

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 30.0

[engine]
fuel_flow_idle_kg_s = 0.10
fuel_flow_mil_kg_s  = 0.90
fuel_flow_ab_kg_s   = 3.20
spool_time_s        = 5.0

[engine.mil_thrust]
mach   = [0.0, 0.3, 0.6, 0.9, 1.2, 1.5, 1.8]
alt_km = [0, 3, 6, 9, 12, 15]
values = [
    60.0, 51.0, 42.0, 33.0, 24.0, 15.0,
    63.0, 54.0, 44.0, 35.0, 25.0, 16.0,
    66.0, 56.0, 47.0, 37.0, 27.0, 17.0,
    68.0, 58.0, 48.0, 38.0, 28.0, 18.0,
    66.0, 56.0, 47.0, 37.0, 26.0, 17.0,
    62.0, 53.0, 44.0, 35.0, 25.0, 16.0,
    56.0, 48.0, 40.0, 32.0, 23.0, 15.0,
]

[engine.ab_thrust]
mach   = [0.0, 0.3, 0.6, 0.9, 1.2, 1.5, 1.8]
alt_km = [0, 3, 6, 9, 12, 15]
values = [
    96.0, 82.0, 68.0, 54.0, 39.0, 24.0,
   101.0, 87.0, 72.0, 57.0, 41.0, 25.0,
   106.0, 91.0, 75.0, 60.0, 43.0, 27.0,
   110.0, 94.0, 78.0, 62.0, 45.0, 28.0,
   106.0, 91.0, 75.0, 60.0, 43.0, 27.0,
    99.0, 85.0, 70.0, 56.0, 40.0, 25.0,
    90.0, 77.0, 64.0, 51.0, 37.0, 23.0,
]

```

---

## Worked example — F/A-18C Hornet

*Values annotated with source. This is a representative starting point, not a certified
simulation.*

```toml
[aircraft]
name         = "F/A-18C Hornet"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = true        # F/A-18C has full authority digital FCS (FLCS)
cruise_alt_m = 12192       # ~40 000 ft — published cruise altitude
mesh         = "fa18c"
cockpit      = "fa18c_hud"

[flight_model]
# Source: Jane's All the World's Aircraft 1998–99
mass_kg      = 10455.0     # operating empty weight (Jane's: 23 050 lb)
wing_area_m2 = 46.5        # Jane's: 400 ft²
wingspan_m   = 12.3        # Jane's: 40 ft 4 in
mac_m        = 3.51        # ~27% of root chord
fuel_kg      = 6531.0      # Jane's: 14 400 lb internal fuel
# Source: NASA TM-86694, estimated from geometry
ixx_kg_m2    = 14651.0
iyy_kg_m2    = 73084.0
izz_kg_m2    = 82804.0

[aero.cl_table]
# Source: NASA TM-86694 (wind-tunnel), extended with thin-aerofoil theory
alpha  = [-5, 0, 5, 10, 15, 18, 20, 25, 35]
mach   = [0.2, 0.4, 0.6, 0.8, 0.9, 1.1, 1.4, 1.8]
values = [
    -0.25,-0.27,-0.28,-0.30,-0.28,-0.20,-0.15,-0.10,
     0.08, 0.09, 0.09, 0.10, 0.09, 0.07, 0.05, 0.03,
     0.47, 0.52, 0.54, 0.58, 0.55, 0.40, 0.30, 0.20,
     0.85, 0.95, 0.98, 1.05, 1.00, 0.73, 0.55, 0.37,
     1.18, 1.32, 1.36, 1.46, 1.38, 1.01, 0.76, 0.51,
     1.30, 1.45, 1.50, 1.61, 1.52, 1.11, 0.84, 0.56,
     1.22, 1.36, 1.40, 1.51, 1.43, 1.04, 0.79, 0.53,
     0.95, 1.06, 1.09, 1.17, 1.11, 0.81, 0.61, 0.41,
     0.60, 0.67, 0.69, 0.74, 0.70, 0.51, 0.39, 0.26,
]

[aero.drag_polar]
# Source: NASA TM-86694, back-calculated from F/A-18 max speed data
cd0           = 0.0197     # clean configuration
k             = 0.128      # k = 1/(π × AR × e); AR = 3.25, e ≈ 0.78
speedbrake_cd = 0.065      # F/A-18 speedbrake (split flap on fuselage)
gear_cd       = 0.028

[aero.cd_wave]
# Source: back-calculated from F/A-18 subsonic/supersonic thrust-drag balance
mach   = [0.70, 0.80, 0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.20, 1.50]
values = [0.000, 0.002, 0.009, 0.024, 0.036, 0.033, 0.024, 0.016, 0.007, 0.002]

[aero.moments]
# Source: NASA TM-86694
cm_alpha = -0.62
cm_q     = -11.4
cm_de    = -1.08
cl_beta  = -0.092
cl_p     = -0.418
cl_da    =  0.082
cn_beta  =  0.124
cn_r     = -0.164
cn_dr    = -0.063

[aero.limits]
alpha_stall_deg  =  18.0
max_g_structural =   7.5   # Jane's: +7.5 g limit (FCS-enforced)
min_g_structural =  -3.0
max_mach         =   1.8

[aero.controls]
# Source: F/A-18 NATOPS
max_elevator_deg = 24.0
max_aileron_deg  = 42.0    # F/A-18 LEX flap + aileron combined
max_rudder_deg   = 30.0

[engine]
# Source: Jane's (F404-GE-402 engine × 2)
fuel_flow_idle_kg_s = 0.14
fuel_flow_mil_kg_s  = 1.21   # Jane's: 9 590 lb/hr each = 19 180 total ÷ 7936 ≈ 2.42 / 2 per eng
fuel_flow_ab_kg_s   = 4.35   # estimated 3.6× MIL
spool_time_s        = 4.5

[engine.mil_thrust]
# Source: Jane's (GE F404-GE-402: 10 900 lbf dry per engine → 48.5 kN × 2 = 97 kN total)
mach   = [0.0, 0.3, 0.6, 0.9, 1.2, 1.5, 1.8]
alt_km = [0, 3, 6, 9, 12, 15]
values = [
     97.0,  83.0,  69.0,  55.0,  40.0,  25.0,
    102.0,  88.0,  72.0,  58.0,  42.0,  26.0,
    107.0,  92.0,  76.0,  61.0,  44.0,  28.0,
    110.0,  95.0,  78.0,  63.0,  46.0,  29.0,
    107.0,  92.0,  76.0,  61.0,  44.0,  27.0,
    100.0,  86.0,  71.0,  57.0,  41.0,  26.0,
     91.0,  78.0,  65.0,  52.0,  38.0,  24.0,
]

[engine.ab_thrust]
# Source: Jane's (GE F404-GE-402: 17 700 lbf AB per engine → 78.7 kN × 2 = 157.4 kN total)
mach   = [0.0, 0.3, 0.6, 0.9, 1.2, 1.5, 1.8]
alt_km = [0, 3, 6, 9, 12, 15]
values = [
    157.0, 135.0, 113.0,  91.0,  66.0,  41.0,
    165.0, 142.0, 119.0,  96.0,  69.0,  43.0,
    173.0, 148.0, 125.0, 101.0,  73.0,  46.0,
    179.0, 153.0, 129.0, 104.0,  75.0,  47.0,
    173.0, 148.0, 125.0, 101.0,  73.0,  46.0,
    162.0, 139.0, 117.0,  94.0,  68.0,  43.0,
    148.0, 127.0, 107.0,  86.0,  62.0,  39.0,
]

[carrier]
# Source: F/A-18C NATOPS flight manual
approach_m_s     = 69.4   # 135 kts
approach_aoa_deg =  8.1
cat_min_m_s      = 66.9   # 130 kts
hook_length_m    =  4.88  # 16 ft

[refueling]
type          = "drogue"   # probe-and-drogue (F/A-18 uses a retractable probe)
max_rate_kg_s = 1.7        # ~220 lb/min typical for probe-drogue


```

---

## Worked example — Tu-95MS Bear (turboprop bomber)

```toml
[aircraft]
name         = "Tu-95MS Bear-H"
type         = "bomber"
engine_type  = "turboprop"
has_fbw      = false
cruise_alt_m = 9000      # ~30 000 ft — published cruise altitude
mesh         = "tu95ms"
cockpit      = "tu95ms_hud"

[flight_model]
# Source: Jane's All the World's Aircraft 1998–99
mass_kg      = 90000.0   # operating empty weight (Jane's: 198 000 lb)
wing_area_m2 = 311.0     # Jane's: 3 349 ft²
wingspan_m   =  50.04    # Jane's: 164 ft 2 in
mac_m        =   6.2
fuel_kg      = 87000.0   # Jane's: max fuel
ixx_kg_m2    = 3000000.0 # estimated; large aircraft
iyy_kg_m2    = 8000000.0
izz_kg_m2    = 8500000.0

[aero.cl_table]
# Generic large swept-wing aircraft shape; lower max CL than a fighter
alpha  = [-4, 0, 4, 8, 12, 16, 18, 22]
mach   = [0.2, 0.4, 0.6, 0.75, 0.82]
values = [
    -0.15,-0.16,-0.17,-0.18,-0.15,
     0.04, 0.04, 0.05, 0.05, 0.04,
     0.30, 0.33, 0.38, 0.40, 0.34,
     0.56, 0.62, 0.71, 0.75, 0.63,
     0.80, 0.89, 1.02, 1.08, 0.91,
     0.99, 1.10, 1.27, 1.34, 1.13,
     1.03, 1.14, 1.32, 1.40, 1.18,
     0.82, 0.91, 1.05, 1.11, 0.93,
]

[aero.drag_polar]
cd0           = 0.024     # higher cd0 due to large fuselage and contra-prop pods
k             = 0.040     # very high AR (AR ≈ span²/area ≈ 8.1), low k
speedbrake_cd = 0.040
gear_cd       = 0.040     # large gear on a heavy bomber

[aero.moments]
cm_alpha = -0.55
cm_q     = -14.0
cm_de    = -0.90
cl_beta  = -0.05
cl_p     = -0.30
cl_da    =  0.04
cn_beta  =  0.08
cn_r     = -0.10
cn_dr    = -0.04

[aero.limits]
alpha_stall_deg  =  16.0
max_g_structural =   2.5   # large bomber structural limit
min_g_structural =  -1.0
max_mach         =   0.82  # Jane's: Mach 0.82 max

[aero.controls]
max_elevator_deg = 20.0
max_aileron_deg  = 15.0
max_rudder_deg   = 25.0

[prop]
# Tu-95 uses contra-rotating 8-blade propellers (AV-60N) — net torque ≈ 0
rotation      = "contra"
torque_factor = 0.0
gyro_factor   = 0.0

[engine]
# Source: Jane's (NK-12MV turboprop × 4: 11 033 kW each)
# Prop thrust falls with speed — modelled in the thrust table
fuel_flow_idle_kg_s = 0.80
fuel_flow_mil_kg_s  = 5.60   # combined for all 4 engines at cruise
fuel_flow_ab_kg_s   = 5.60   # no AB; field ignored
spool_time_s        = 8.0    # turboprop spool is slower than turbofan

[engine.mil_thrust]
# Prop thrust: high at static, falls with airspeed
# Source: estimated from published max speed + drag balance; NACA TN 1339 prop efficiency
mach   = [0.0,  0.3,  0.5,  0.65, 0.75, 0.82]
alt_km = [0,    3,    6,    9,   12]
values = [
    880.0, 680.0, 560.0, 460.0, 370.0,  # Mach 0.0
    640.0, 490.0, 400.0, 330.0, 265.0,  # Mach 0.3
    510.0, 390.0, 320.0, 265.0, 210.0,  # Mach 0.5
    440.0, 340.0, 278.0, 230.0, 183.0,  # Mach 0.65
    400.0, 307.0, 251.0, 208.0, 165.0,  # Mach 0.75
    370.0, 284.0, 232.0, 192.0, 152.0,  # Mach 0.82
]

[refueling]
type          = "drogue"
max_rate_kg_s = 2.0

```
