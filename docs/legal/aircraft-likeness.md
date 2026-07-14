# Aircraft likeness and reference-source policy

**Status:** project policy. Applies to this repository and to every content pack published under the
`fighters-legacy` organisation, including `fl-base-pack`.

This document answers four questions, and it is written before the first aircraft ships rather than
after — a policy written afterwards can only ratify whatever was already done.

1. May I model a real aircraft type?
2. From what references?
3. With what markings?
4. What must I document?

---

## 1. Real aircraft types may be modelled

**Yes.** Fighters Legacy models real aircraft, names them by their real designations, and uses their
real published performance. The docs already work through the F-15C, the F/A-18C and the Tu-95MS as
examples, and the first bundled aircraft is a Northrop F-5E Tiger II.

What makes this defensible is not that the aircraft are fictional. It is **where the data comes
from**, and the answer must be: the public record.

## 2. Public-domain government sources only

Permitted references:

- Declassified 3-view drawings and general-arrangement diagrams.
- USAF / US Navy / NASA photography (works of the US federal government, public domain).
- Published **Standard Aircraft Characteristics** charts.
- Declassified flight manuals and technical orders.
- NASA and NACA technical reports (`ntrs.nasa.gov`).
- Manufacturer marketing material *for dimensions only*, cross-checked against a government source.

**Forbidden, without exception:**

- **No mesh traced from, derived from, converted out of, or "cleaned up" from another simulator,
  game, or commercial 3D model** — regardless of how the file was obtained, what licence its uploader
  claimed, or how much it was modified afterwards. This restates the rule already in
  `fl-base-pack/CONTRIBUTING.md`, and makes explicit that it applies to the **airframe geometry**,
  not merely to the livery.
- **No aerodynamic or performance values lifted from another simulator's data files.** Not from
  DCS, not from BMS, not from a Flight Simulator add-on, not from a wiki that copied one of them.
- No reference material marked export-controlled (ITAR/EAR), classified, or "distribution limited".
  If a document says you may not redistribute it, you may not derive from it either.

## 3. Aerodynamic values are derived, not copied

For most aircraft — and for **every** trainer and light fighter — no public source publishes the
stability derivatives. That is a real constraint, and the answer is not to go and find someone else's
tuning file.

**Derive them.** Compute the moment derivatives from the geometry using USAF DATCOM methods, then
calibrate the result against published performance charts: stall speed, max level Mach, rate of climb,
sustained turn rate, fuel burn. `fm-trim --expect` exists precisely so that this calibration is a
mechanical, checkable step rather than a matter of taste — the model must reproduce the aircraft's
published numbers, and CI fails it when it stops doing so.

This is the same clean-room standard the engine already holds itself to with respect to Jane's
Fighters Anthology: *design lesson only, no code or tuning values copied*
(`docs/architecture.md`). The content is held to it too.

## 4. Markings and liveries

- **No unit markings, squadron insignia, nose art, or operator liveries** without documented
  clearance from the rights holder.
- Default to generic, fictional, or unmarked schemes. National insignia on historical aircraft in
  their historical context is acceptable; a current operator's squadron badge is not.
- Manufacturer logos are not reproduced on the airframe.

## 5. Every aircraft ships a `SOURCES.md`

This is a hard requirement, and it is what makes the rest of this policy auditable rather than
aspirational.

Each aircraft directory contains a `SOURCES.md` in which **every geometric dimension, mass, thrust
figure and aerodynamic value is cited to a specific public document**, with a URL where one exists.
Values that were *derived* (DATCOM, curve fits, calibration against a chart) say so, and name the
inputs they were derived from. The pull request that adds the aircraft links the sources.

If a number cannot be cited or derived, it does not ship.

## 6. Trademarks

Aircraft type names, designations, and manufacturer names are the trademarks of their respective
owners. Fighters Legacy uses them **nominatively** — to identify the aircraft being modelled, which
is what they are for. The project claims no affiliation with, sponsorship by, or endorsement from any
aircraft manufacturer or armed force, and the game's branding does not suggest otherwise.

See also the project's existing disclaimer in `README.md`.

## 7. The fallback

If this policy ever proves too permissive in a specific case, the fallback is not to abandon the
project's realism: `tools/blender_gen.py` generates entirely fictional, IP-free parametric airframes,
and the engine is fully content-agnostic. A pack of fictional aircraft is a supported configuration.
We do not expect to need it, but the option is deliberately kept open.

---

**Questions, or an aircraft you are unsure about?** Open an issue before you start modelling. It is a
cheap conversation, and it is much cheaper than a rejected pull request with a hundred hours of work
in it.
