# Weapons & Sensors Authoring Guide

This guide covers the **combat systems** an aircraft or ground unit brings to a fight: what it can
**see** (sensors), what it looks like to everyone else (signatures), how friend-from-foe is decided
(IFF), what it shoots (weapon seekers), and the electronic-warfare gear that bends all of the above
(chaff, flare, ECM/ECCM). It is the companion to [formats.md](formats.md), which carries the exhaustive
per-field TOML tables; this document explains how the pieces fit together and how to tune them so a
theater plays the way you intend.

It is the deliverable of Epic F (combat sensors, datalink & EW). The wire side is documented in
[network-protocol.md](../developer/network-protocol.md); the validators (`validate-sensor`, `validate-weapon`,
`validate-entity`) enforce the rules below at pack-build time.

---

## The one-vocabulary model

There is exactly **one** description of "what can this thing see": a **sensor def** (`sensors/*.toml`).
The same schema serves three consumers — a pilot's avionics, an AI's detection, and a missile's
seeker head. A radar, an IRST, a laser designator, and the human eyeball are all *a cone, a range
band, and a probability of seeing something in it*; they differ only in their numbers.

Sensing has two sides, and they are deliberately independent:

| Side | Authored on | What it says |
|------|-------------|--------------|
| **Observer** | the sensor def (`sensors/*.toml`), listed in the entity's `sensors` suite | how far and how reliably this thing can detect |
| **Target** | `[signatures]` on the **entity** def | how loud this thing is to each kind of sensor |

An entity that declares **no** `sensors` is not blind and not omniscient — it gets the builtin
**eyeball** (short range, visual only, no lock). Honest sensing is the default, not an opt-in.

> **Key rule:** a consumer never gets ground truth. It gets a **contact** — a last-known track that
> may be stale, coasting, or wrong. You cannot author your way to a wallhack.

---

## Signatures — the target side

`[signatures]` on an entity def scales how far each sensor channel detects it, as **unitless
multipliers against a baseline fighter (1.0)**:

```toml
[signatures]
rcs    = 0.3   # radar cross-section; seen by type = "radar"
ir     = 1.2   # infrared/heat;       seen by type = "ir"
visual = 1.0   # optical size;        seen by type = "visual"
laser  = 1.0   # laser reflectivity;  seen by type = "laser"
```

Radar detection range scales by **`sqrt(rcs)`** (the square root echoes the fourth-power radar
equation — `rcs = 0.01` is seen at a *tenth* of baseline range, not a hundredth); IR, visual and
laser scale **linearly**. A hot, low-RCS strike aircraft is the natural expression of these four
numbers: hard to find on radar, easy on IR.

Per-crew acquisition quality is separate from the hardware — see `[ai]` skill/reaction in
[formats.md](formats.md).

---

## Sensor defs — the observer side

A sensor def lives in `sensors/<id>.toml`. Ranges are **authored in nautical miles**, stored SI.

```toml
[sensor]
id              = "fl-base:apg66"
name            = "AN/APG-66 fire-control radar"
type            = "radar"          # visual | ir | radar | laser
omnidirectional = false            # true = no cone (an RWR, a missile-approach warner)
emitter         = true             # it announces itself — hostile RWR can hear it (see below)
role            = "aircraft"       # aircraft | seeker (tooling only; a seeker head is a sensor too)
eccm            = 0.3              # 0..1 resistance to noise jamming (#529); extends burn-through

[search]                           # required: how a target is FOUND (wide, low PoD)
az_half_angle_deg = 60.0
el_half_angle_deg = 30.0
min_range_nm      = 0.5
max_range_nm      = 30.0
pod               = 0.35           # probability of detection PER CHECK at 10 Hz

[track]                            # optional: how a target is HELD (narrow, high PoD). Absent = search-only
az_half_angle_deg = 55.0
el_half_angle_deg = 25.0
min_range_nm      = 0.3
max_range_nm      = 22.0
pod               = 0.70
lock_hold_s       = 4.0            # coast time after the target leaves the cone before the track drops
```

- **`pod` is per check at the reference 10 Hz cadence** (`[world] sensor_check_hz`). A PoD without a
  rate is meaningless — tune against 10 Hz and an operator who changes the cadence changes effective
  acquisition time (honestly, never silently renormalized).
- **PoD gates acquisition; geometry maintains.** A die is rolled only to *find* a target; a held
  contact stays held while it is in the cone, and coasts for `lock_hold_s` when it leaves. A search
  radar's `track` lobe is what turns a Detected blip into a Locked track.
- **A search-only sensor cannot lock.** The eyeball has no `[track]`; it finds an aircraft, it never
  holds a firing-quality lock on one.

Weather degrades the four channels differently (`#209`): the visual channel is hit hardest and is the
only one the dark touches; IR shrugs off darkness (a jet is as hot at midnight); radar barely notices
weather at all. Fair weather at noon costs **exactly nothing** — every `pod` is quoted against it.

`validate-sensor` warns on the usual authoring mistakes: a passive sensor that `emit`s, a non-emitting
radar carrying a `[track]` lobe it can never use, or a track lobe wider/longer/more reliable than the
search lobe that feeds it.

---

## Radar modes, STT & the RWR (#526)

`emitter = true` radars are *operated* at runtime in one of four modes (the pilot cycles them; the AI
and missions can set them). Modes govern **radar-typed sensors only** — an IRST or the eyeball is
passive and unaffected:

| Mode | Emits? | Behavior | Hostile RWR reads |
|------|--------|----------|-------------------|
| **Silent** | no | EMCON — the radar detects **nothing** it does not radiate. IRST/eyeball still work. | nothing |
| **Search** | yes | Sweeps and reports a **bearing** (Detected); never a lock. | a search strobe |
| **TWS** | yes | Track-while-scan: holds **soft** tracks on everything in the cone (can reach Locked, but not firing-quality). | a scan |
| **STT** | yes | Single-target-track: dedicates the beam to **one** designated target, a **firing-quality** lock. | a steady lock tone |

Only an **STT** lock is *firing-quality* (`Contact::firingQuality`) — the continuous illumination a
semi-active radar (SARH) shot needs. TWS gets you a track; STT gets you a shot.

The **RWR** is the honest inverse of all this: when an *emitting* radar (or laser) holds **you** as a
contact, **your** RWR lights up with the emitter's bearing — a strobe for a scan, a lock tone for an
STT lock. A radar you have switched to Silent is invisible to the enemy's RWR. There is nothing to
author here beyond `type`/`emitter` on the sensor def; the behavior falls out of the emissions flag.

---

## IFF / identification (#527)

Every contact carries an **identification** the observer has actually earned — **not** the target's
true faction, which would be an identification wallhack:

| Ident | When |
|-------|------|
| **Friend** | the contact answers an IFF interrogation with a friendly code — i.e. its faction is friendly (same coalition) to yours. Friendlies squawk, so a friend is known the moment it is detected, at any range. |
| **Foe** | the contact is hostile **and positively identified** — you have eyes on it (a visual contact) **or** a committed firing-quality (STT) lock. |
| **Unknown** | detected, but not a friend and not a positively-identified foe — a distant hostile you have not merged with, or a neutral. |

So a bandit at range is an ambiguous **Unknown** blip until you visually ID it or lock it up — the
commit loop BVR combat turns on, and the reason a careless missile at an Unknown can be a
friendly-fire kill. Coalitions are configured by the mission's faction relationship matrix; before a
mission loads, distinct non-zero factions are hostile and faction 0 is neutral (has no enemies).

Authoring: set each entity's team via the mission's faction assignment (see
[missions.md](missions.md)). Nothing about IFF is authored on the sensor or weapon defs — it is a
property of who is looking at whom.

---

## Datalink / shared team track picture (#528)

Every pilot receives a **fused** picture of what their whole team can see: their own sensor contacts
plus every same-faction teammate's, deduplicated by target. You see the bandit your wingman locked
even if your own radar never found it. Each shared track is coloured by its IFF identification and
marked own-sensor (you hold it too) vs datalink-only (only a teammate does). This is delivered as
`MsgDatalink` and drawn on the HUD radar scope + RWR — nothing to author; it works for any faction
with more than one set of eyes.

---

## Weapon seekers (#583)

A missile's seeker **is a sensor**: `[seeker] sensor_id` references a sensor def (with
`role = "seeker"`), and the missile evaluates it through the same detection math as any aircraft
radar. What stays on the weapon is *employment doctrine*, not sensing:

```toml
[seeker]
type            = "semi-active-radar"  # active-radar | semi-active-radar | ir (or infrared) | laser | gps | anti-radiation | unguided
sensor_id       = "fl-base:aim7m-seeker"
fire_and_forget = false                # false = the launch platform must keep supporting the shot (STT)
```

**The seeker `type` vocabulary is hyphenated**, and `parseWeaponDef` throws on anything else — so an
underscored `semi_active_radar` does not load. The one exception is infrared, which accepts both `ir`
and `infrared`.

- A **SARH** (`semi-active-radar`) shot rides the **shooter's** contact table — it needs the shooter
  to hold the target in **STT** (firing-quality illumination). Drop the lock and the shot goes dumb.
- An **ARH** (`active-radar`) shot is supported until *pitbull*, then its own radar takes over. The
  handover range is **`pitbull_nm`**, in nautical miles — range-to-go, **not** a `_m` field:

      [seeker]
      type       = "active-radar"
      sensor_id  = "fl-base:aim120c-seeker"
      pitbull_nm = 10                    # own radar goes active (and emitting) inside 10 nm to go

  `0` (the default) means active straight off the rail, and the field is rejected on any seeker
  that is not `active-radar` — going active *is* the pitbull.
- An **IR** shot is fire-and-forget.

See [formats.md](formats.md#weapon-data--toml) for the full `[seeker]`/`[performance]`/`[warhead]`
tables.

---

## Electronic warfare: chaff, flare & ECM (#529)

### Countermeasure susceptibility — on the WEAPON

How defeatable a missile is by expendables is authored on the weapon, as fractions in `[0, 1]`:

```toml
[countermeasures]
chaff_susceptibility = 0.5   # 0 = immune to chaff, 1 = always defeated (radar seekers)
flare_susceptibility = 0.5   # (infrared seekers)
notch_susceptibility = 0.3   # defeated by a beam/notch manoeuvre against a doppler seeker
```

A **flare** decoys an **IR** seeker; **chaff** decoys a **radar** one — the channels are not
interchangeable, which is exactly why the RWR (knowing *what* is shooting at you) matters. When a
matching-channel decoy is close to the target and the seeker's susceptibility roll passes, the lock
breaks: the missile coasts on its last-known track and reacquires by geometry once the decoy has
fallen behind. A well-timed pop with a break turn is what defeats a shot.

### Countermeasure magazines & ECM — on the ENTITY

An aircraft carries expendables and (optionally) a jammer:

```toml
[entity]
# ...
chaff_count = 30    # radar-decoy rounds; 0 = no chaff dispenser (the default)
flare_count = 30    # ir-decoy rounds;    0 = no flare dispenser
```

- **Dispensing** (player key **E**) pops one chaff + one flare per press until the magazines run dry.
- **ECM** (player key **J**) is a runtime toggle, not a magazine — an aircraft switches its jammer on
  and off; there is nothing to run out of. A jamming target **denies a hostile radar a lock** beyond
  its *burn-through range*: the radar still gets a bearing strobe, but no firing solution until it
  closes inside where the skin return beats the noise. A radar's **`eccm`** (on its sensor def)
  extends that burn-through — a good radar burns through farther out; a poor one must merge.

Which aircraft carry a jammer is content policy: give a dedicated EW platform a high `eccm` radar and
generous `chaff_count`/`flare_count`, and give a strike aircraft flares to survive the IR threat.

---

## Worked example: a radar fighter that survives the fight

`entities/mig-29.toml` (excerpt):

```toml
[entity]
id          = "fl-base:mig-29"
max_hp      = 100
chaff_count = 30
flare_count = 60
sensors     = ["fl-base:n019", "fl-base:eyeball_day"]   # what it can see WITH

[signatures]                                            # what it can be seen AS
rcs = 3.0   # a big radar target...
ir  = 1.5   # ...and a hot one
```

**`sensors` belongs to `[entity]`, so it must be written above the `[signatures]` header.** A bare
key after a table header is scoped *into* that table by TOML, and `EntityDefParser` reads
`entity["sensors"]` — an aircraft whose suite landed in `[signatures]` would fly with the builtin
eyeball, which is the "builtin-radar fallback" the red-air acceptance criteria forbid. `[signatures]`
and `[ai]` have closed field sets, so the parser now **rejects** any other key in them by name
rather than dropping it; `validate-entity` reports the same error.

`sensors/n019.toml` (a capable radar with modest ECCM):

```toml
[sensor]
id      = "fl-base:n019"
name    = "N019 Sapfir radar"
type    = "radar"
emitter = true
eccm    = 0.25

[search]
az_half_angle_deg = 65.0
el_half_angle_deg = 30.0
min_range_nm      = 0.5
max_range_nm      = 35.0
pod               = 0.35

[track]
az_half_angle_deg = 60.0
el_half_angle_deg = 28.0
min_range_nm      = 0.3
max_range_nm      = 26.0
pod               = 0.72
lock_hold_s       = 4.0
```

This aircraft flies TWS to build the picture, goes STT to take a radar shot (and lights the target's
RWR with a lock tone), pops chaff when *it* is locked, and flares against an IR shot — the whole EW
loop from a handful of numbers.

---

## Turret mounts vs hardpoint stations (#966/#970)

A **hardpoint station** says *what an airframe carries* (`[[hardpoints]]`, above). A **turret mount**
gives one or more of those stations an *aiming direction independent of the airframe nose* — a
defensive gun that tracks a chaser, or a static SAM launcher that elevates. Turrets are authored on
the entity as `[[turrets]]` and bound to a crew seat via the seat's `turret =` field (see the
[`[[crew]]` and `[[turrets]]` section in formats.md](formats.md#crew-and-turrets-optional--multi-crew-seats-and-turret-mounts)).

Two facts about how a turret fires:

- **The slew is server-authoritative.** The server slews the turret toward its commanded direction at
  the mount's `slew_rate_deg_s`, clamped to the `az_*`/`el_*` limits — there is no instant aim,
  regardless of what a client reports. The same pure `stepTurret` servo runs on the client purely as a
  smoothing predictor; the server pose is the truth.
- **The shot leaves along the turret bore**, not the airframe nose — a gun burst or a store released
  from a turret-mounted station fires along the current turret orientation. A nose-mounted station is
  unchanged (bit-identical to a single-seat fighter).

This is also the mechanism that gives a **static air-defense emplacement launcher elevation** (the
gap documented as owed to #585): mount the launcher as a turret and the SAM/AAA controller slews it
onto the lead point instead of firing along a fixed nose.

## See also

- [formats.md](formats.md) — the exhaustive per-field TOML tables for sensors, weapons, and entities.
- [network-protocol.md](../developer/network-protocol.md) — `MsgDatalink`, `MsgClientInput` EW bits, the
  `SnapshotEffects`/`CountermeasureRelease` cosmetic channel.
- [missions.md](missions.md) — faction assignment and the relationship matrix that drives IFF.
- [controls.md](../user-guide/controls.md) — the pilot key map (R radar mode, E dispense, J ECM).
- `validate-sensor`, `validate-weapon`, `validate-entity` — pack-build validators for everything above.
