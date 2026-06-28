# Prior Art & Competitive Landscape

This document surveys existing flight simulators — open-source and commercial — and explains
where fighters-legacy fits relative to them. It is intended for contributors who want to
understand the motivations behind key design decisions before diving into the codebase.

For canonical upstream documentation on the technologies the engine uses, see
[`docs/references.md`](references.md) (issue #80). This document is motivation and landscape;
that one is a tech reference index.

---

## Open-Source Flight Simulators

### FlightGear

The most mature open-source flight simulator (GPL, C++, active since ~1997). Uses JSBSim or
YASim for flight dynamics and OpenSceneGraph for rendering. Broad aircraft and scenery
coverage, active community, cross-platform.

Key contrasts with fighters-legacy:
- Civil and general aviation focus; combat systems are bolted on via add-ons rather than
  first-class engine features.
- Content is bundled tightly with the engine — there is no clean `IContentPack`-style
  separation boundary.
- OpenGL renderer; no Vulkan path.
- Enormous accumulated scope (weather, ATC, multiplayer, terrain) with a codebase that
  reflects decades of organic growth.

FlightGear shows that a GPL cross-platform flight sim can sustain a long-lived community.
Its architecture is not a model to follow, but its community health is.

### JSBSim

Not a full simulator — an open-source (LGPL) flight dynamics model library, usable standalone
or embedded. Best-in-class FDM with published methodology and extensive documentation.

JSBSim is a reference for fighters-legacy's flight model design, not a dependency. The
stability-derivative coefficient structure and AoA/Mach-indexed table approach borrow from
JSBSim's mathematical skeleton. What differs: fighters-legacy uses TOML rather than JSBSim's
FDL/XML format, carries no systems simulation layer, and is integrated from scratch. See the
FDM RFC issue for the full design rationale.

### Vega Strike

Open-source (GPL, C++) space combat engine. Philosophically the closest to fighters-legacy
in architecture: content-agnostic engine, engine/data separation, mod-friendly design. The
domain is space rather than atmospheric combat, but the structural thinking is similar.

### OpenFalcon / FreeFalcon / Falcon 4 BMS

Community forks of the Falcon 4.0 source code that Microprose briefly released in the early
2000s. These require the original proprietary Falcon 4 data files, are not cleanly
redistributable, and are effectively Windows-only. BMS in particular is the state of the art
for open F-16 simulation — but it is legally a grey area and not a foundation anyone can
build on freely.

---

## Commercial and Closed-Source Flight Simulators

### Jane's Combat Simulations series (Electronic Arts / Origin, 1994–2002)

The direct spiritual ancestor of fighters-legacy. Jane's Fighters Anthology (1998) was a
compilation of USNF '97, ATF Gold, and Marine Fighters on a unified engine, covering modern
Cold War jets (F-15, F/A-18, Su-27, MiG-29) at a fidelity level between arcade and hardcore.

The series is now abandonware. The engine is Windows 9x/NT-era and barely runs on modern
hardware. fighters-legacy is an attempt to build an open-source, cross-platform, Vulkan-based
successor to this specific game lineage.

### Falcon 4.0 (Microprose, 1998)

The other titan of the 1990s combat sim era. F-16-only but legendary for its dynamic campaign
and avionics fidelity. Source was partially released, spawning BMS (see above). More
hardcore than Jane's; the audience overlap is significant. The dynamic campaign model in
Falcon 4 is a reference point for fighters-legacy's campaign architecture.

### DCS World (Eagle Dynamics, free base + paid modules, Windows-only)

The current gold standard for modern combat flight simulation. Extreme per-aircraft fidelity
(clickable cockpits, full realistic systems), DirectX 11/12, module-based DLC. The
philosophical opposite of fighters-legacy: closed, Windows-locked, maximally coupled content
and engine. But it represents the fidelity ceiling the genre can aim at, and its community
documentation on flight model concepts is a useful reference.

### IL-2 Sturmovik: Great Battles (1C / 777 Studios)

WWII-focused, slightly more accessible than DCS, DirectX 11, strong VR support. A different
era (WWII rather than Cold War jets) than fighters-legacy targets, but a useful benchmark for
visual and flight model quality achievable with a small-to-mid team.

### Strike Fighters 2 (ThirdWire)

The architecturally closest commercial comparison. ThirdWire built a deliberately modular
engine: aircraft, terrain, and campaigns are loaded from data files rather than compiled in.
The mod community is large and active. Still closed-source and Windows-only, and the engine
is aging (DirectX 9). Strike Fighters 2's content-separation philosophy mirrors
fighters-legacy's `IContentPack` design more than anything else in the commercial space.

### X-Plane 12 (Laminar Research)

Blade-element-theory FDM — arguably the most physically accurate flight model of any
commercial simulator. Vulkan/Metal renderer, real developer SDK, primarily civil aviation.
X-Plane's published FDM methodology is a useful reference even though fighters-legacy does not
use blade element theory (see the FDM RFC issue for why). X-Plane demonstrates that a
Vulkan-native flight simulator is viable and maintainable across platforms.

### Microsoft Flight Simulator 2024 (Asobo / Microsoft)

DirectX 12, Azure-backed photogrammetry terrain, enormous budget, civil aviation only. Sets
the visual bar for what is possible with unlimited resources. Not a combat sim; not a
competitive comparison. Relevant only as a benchmark for terrain streaming ambition.

### Prepar3D (Lockheed Martin)

FSX lineage, used for professional and military training contracts. EULA prohibits
entertainment use. Demonstrates institutional demand for simulation fidelity in military
aviation — demand that fighters-legacy could eventually address in a way that is legally open.

---

## Where fighters-legacy Sits

| | DCS World | Strike Fighters 2 | Jane's FA (1998) | fighters-legacy |
|---|---|---|---|---|
| License | Proprietary + DLC | Proprietary | Abandonware | GPL v3 |
| Content model | Tightly coupled modules | Modular data files | Monolithic | `IContentPack` plugin interface |
| Platform | Windows only | Windows only | Windows only | Win / Linux / macOS |
| Renderer | DirectX 11/12 | DirectX 9 | Software / D3D5 | Vulkan 1.3 |
| Era / style | Modern, ultra-fidelity | Cold War jets | Cold War jets | Cold War jets (FA-inspired) |
| Moddability | Lua scripting | Strong, unofficial | None | First-class, by design |
| Maturity | 20+ years | 15+ years | Defunct | Phase 1 |

The gap fighters-legacy fills is real: no GPL, cross-platform, Vulkan-based combat flight sim
engine exists with a clean content separation layer. DCS is the fidelity king but is closed,
Windows-locked, and paywalled. Strike Fighters 2 has the right modular philosophy but is aging
and proprietary. Jane's Fighters Anthology is the spiritual ancestor but is dead software.

The realistic near-term fidelity ceiling is something closer to Jane's FA or Strike Fighters 2
quality — which is a valuable and currently unoccupied position in the open-source landscape.

---

## Multiplayer at Scale — Prior Art

The 128+ player re-target (decision record 2026-06-28) invites comparison to large-scale
combat-flight multiplayer, a different axis from the single-player-fidelity comparisons above:

- **War Thunder (Gaijin)** — the reference for large combat-air battles, with high player
  counts per match, server-authoritative netcode, accounts/progression, and anti-cheat. Closed
  and free-to-play with monetized progression — the opposite of our self-host, no-progression-
  gate model, but the benchmark for what "many planes in one battle" should feel like.
- **DCS World multiplayer** — dedicated servers with sizeable persistent missions; community-
  run, but limited matchmaking/identity infrastructure and Windows-only servers. Shows the
  appetite for large community-hosted combat servers that fighters-legacy can serve openly and
  cross-platform.
- **Falcon 4 BMS multiplayer** — coordinated mission play over community servers; demonstrates
  the value of a shared track picture / datalink for team coordination (Epic F).

The unoccupied position fighters-legacy targets: a **GPL, cross-platform, self-hostable**
combat flight sim engineered for 128+ players with server-authoritative netcode, pluggable
identity, and Kubernetes-native fleet operations — none of which the closed incumbents offer to
self-hosting communities.

---

## Flight Model Prior Art

The FDM design decision — a simplified 6-DOF stability-derivative model — is documented in
[issue #103](https://github.com/fighters-legacy/fighters-legacy/issues/103). See
[`docs/design.md`](design.md) for the fidelity target that drives it.
