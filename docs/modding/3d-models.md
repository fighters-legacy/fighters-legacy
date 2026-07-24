# 3D Model Authoring Guide

This guide covers the engine conventions for glTF 2.0 aircraft and unit meshes. Follow it
to ensure your models are accepted by `validate-mesh` and render correctly at runtime.

> **Modelling a real aircraft?** Read
> [docs/legal/aircraft-likeness.md](../legal/aircraft-likeness.md) before you start. Real types may be
> modelled, but **only** from public-domain government sources — nothing traced, derived, or converted
> out of another simulator or commercial 3D model — and every aircraft ships a `SOURCES.md` citing
> every dimension to a public document.

---

## Overview

The engine loads `.glb` (binary glTF) and `.gltf` (JSON + separate `.bin`) files as raw bytes
via `FolderContentPack`. The renderer parses glTF 2.0 at load time. All conventions below
exist so the renderer can locate expected nodes and textures without heuristics.

Mesh files live in `aircraft/<id>/` inside the content pack directory.

---

## Coordinate system and winding

The engine uses a **right-handed, Y-up, metric** coordinate system — the glTF 2.0 standard —
with one engine-specific convention: **+X is forward** (the aircraft nose, matching the flight
model's longitudinal axis).

| Axis | Direction |
|---|---|
| **+X** | Forward (nose) |
| **+Y** | Up |
| **+Z** | Starboard (right); −Z = port (left) |

The table above is the engine's internal **body frame** — what the flight model and the sim use.
It is **not** the frame you author in, and you do not have to build "wrong" to satisfy it (#906).

**Authoring orientation — build the natural way.** Model your aircraft with the **nose pointing
along the DCC's natural forward**, and export with Blender's default glTF settings. Blender's
exporter writes Y-up (its *+Y Up* option) and puts the model's nose along the glTF content-forward
axis **+Z**. The engine detects that a mesh came from a content pack and rotates it **+Z → +X** into
the body frame on import (a fixed +90° about +Y — see `platform/MeshOrient.h`). So:

- **Nose** along glTF **+Z** (Blender's natural forward on a default export).
- **Up** along **+Y**.
- No manual yaw, no "forward axis" gymnastics, no hand-correction on export.

The rotation is applied on load; the flight model's own +X-forward body axes are untouched.

**Winding and normals.** Triangles must be wound **counter-clockwise when viewed from outside**
(the glTF 2.0 convention), so face normals point **outward**. Blender produces this by default.
This matters because the engine's opaque pipeline is **single-sided** — back faces are culled. A
mesh with inverted winding renders *inside-out*: from outside it shows only its far interior
faces (or vanishes), and the model's own body becomes visible from the cockpit camera, which sits
at the entity origin and relies on back-face culling to stay hidden.

**Verify in Blender.** Enable **Viewport Overlays → Face Orientation**. Correctly wound (outward)
faces render **blue**; inverted faces render **red**. A finished exterior should be entirely blue.
If you see red, recalculate normals in Edit Mode: **Mesh → Normals → Recalculate Outside**
(`Shift+N`).

**Verify with the pipeline.** `validate-mesh` checks triangle winding against the stored vertex
normals and reports a mesh that is wound inside-out (most faces wound opposite their normals) as an
error, with the same "Recalculate Outside" hint. Run it on your exported `.glb` before shipping.

### Reference meshes

The engine's built-in placeholder shapes (#886 — one per category: aircraft, missile, bomb,
rocket, ground vehicle, naval vessel, structure, the ejected-pilot parachute (#672), plus the
Unknown error beacon; outward normals)
are the canonical reference for **winding, scale and proportion**. They are engine-internal meshes
in the **body frame (+X forward)** — they bypass the content-import rotation — so when you import
them into Blender they will appear **90° rotated** from your own +Z-forward content. That is
expected; the loader applies the +Z → +X mapping to your mesh, not to theirs. Use them to check
that your faces are all-blue (outward) and your scale is right, not to copy the forward axis.

```bash
python3 tools/gen_builtin_glb.py --export-dir /tmp/builtin
# writes /tmp/builtin/builtin_aircraft.glb, builtin_missile.glb, builtin_bomb.glb,
#        builtin_rocket.glb, builtin_ground_vehicle.glb, builtin_naval_vessel.glb,
#        builtin_structure.glb, builtin_parachute.glb, builtin_unknown.glb
#        (+ *_damaged wreck variants, all-blue from outside) and builtin_floor.glb
#        (Y-up ground quad)
```

---

## Toolchain

Blender is the recommended authoring tool.

**Export settings** (File → Export → glTF 2.0):

| Setting | Value |
|---|---|
| Format | **glTF Binary (.glb)** for release; glTF Separate (.gltf + .bin) acceptable for development |
| Include | Selected Objects, UVs, Normals, Tangents |
| Vertex Colors | **Off** — engine does not use them |
| Textures | **Do not embed** — export as separate PNG files and run through `tex-compress` |
| Compression | Off (engine handles this separately via KTX2) |
| Animation | Include if gear/prop/door animations are present |

See [`docs/modding/textures.md`](textures.md) for the PNG → KTX2 texture pipeline.

---

## Node naming conventions

*(Enforced by `validate-mesh`)*

- All node names must be **lowercase with underscores** — no spaces, no uppercase, no hyphens
- The root node name must match the asset ID exactly

```
fa18c              ← root node; matches asset ID
fa18c_fuselage
fa18c_canopy
fa18c_left_wing
fa18c_right_wing
fa18c_left_flap
```

Invalid names that will fail validation: `FA18C`, `fa-18c`, `FA-18C Fuselage`, `Fuselage`.

---

## Damage-state meshes (`_b` suffix)

*(Enforced by `validate-mesh`)*

A battle-damaged variant of node `X` is named `X_b`. Both the base node and its `_b` variant
must coexist in the same `.glb` file. The engine selects between them at runtime based on the
entity's damage threshold.

```
fa18c_fuselage      ← clean
fa18c_fuselage_b    ← battle-damaged
fa18c_left_wing
fa18c_left_wing_b
```

Not every node requires a `_b` variant. Structural nodes (fuselage, wings) benefit most;
small details (cockpit glass, antennas, sensor pods) may omit it without a validation error.
The absence of a `_b` pair produces a warning, not an error.

---

## LOD variants

*(Auto-discovered and validated by `validate-mesh`)*

LOD files are separate `.glb` files with a `_lod<N>` suffix. The base file (no suffix) is the
highest-detail model used in the cockpit view.

| File | Usage | Polygon target |
|---|---|---|
| `fa18c.glb` | In-cockpit and close-pass view | Full detail |
| `fa18c_lod0.glb` | Formation distance (~500–2000 m) | ~50% of base |
| `fa18c_lod1.glb` | Long range (~2000–8000 m) | ~20% of base |
| `fa18c_lod2.glb` | Horizon / large formation | ~5% of base |

LOD files must pass the same conventions as the base file: same node naming convention, same
material structure, no embedded textures. When you run `validate-mesh fa18c.glb`, the tool
automatically discovers and validates `fa18c_lod0.glb`, `fa18c_lod1.glb`, etc. in the same
directory.

LOD files are optional. A model without LOD variants is valid; the engine uses the base mesh
at all distances until LOD files are provided.

---

## Special-purpose meshes

These follow naming conventions but are not validated by `validate-mesh`. They are expected
by the renderer when present.

| File | Purpose |
|---|---|
| `<id>_shadow.glb` | Simplified convex hull used for shadow casting. No materials required. |
| `<id>_cockpit.glb` | Cockpit interior. Must include a node named `camera_anchor` (the pilot eye-point) and the instrument panel geometry. Instruments are non-interactive geometry. |

---

## Variant node-sets

One `.glb` can carry the union of an airframe family's geometry, with each entity def selecting the
node-set it draws. Use it when a variant's silhouette differs visibly — a two-seat trainer canopy, a
reshaped nose, an enlarged spine — but the rest of the airframe is shared. Subtler differences are
better handled with *variant-by-data* (one mesh, forked flight model / entity def / loadout).

Tag a node by putting `fl_variant` in its glTF `extras`, as either a string or an array of strings:

```json
"nodes": [
  { "name": "fuselage", "mesh": 0 },
  { "name": "canopy_single", "mesh": 1, "extras": { "fl_variant": "single_seat" } },
  { "name": "canopy_two",    "mesh": 2, "extras": { "fl_variant": ["two_seat", "trainer"] } }
]
```

The entity def picks one tag:

```toml
[entity]
mesh         = "mig21/mig21"
mesh_variant = "two_seat"
```

Rules:

1. **Untagged nodes are always drawn.** That is the shared airframe, and it is why every mesh
   authored before this feature is unaffected — no `mesh_variant`, no tags, no change.
2. A tagged node is drawn only when the entity's `mesh_variant` appears in its tag list. Excluding a
   node excludes its whole subtree.
3. This is node **presence**, chosen once at load. Node **pose** is
   [articulation](#animation-channels) — a different axis. An animated canopy that opens is still a
   single-seat canopy.
4. `validate-entity --pack` errors when `mesh_variant` matches no tag in the referenced mesh, and
   lists the tags the file does declare. Without that check a typo renders as the bare shared
   airframe with no diagnostic anywhere.
5. Damage variants compose: a tagged node named `<name>_b` is the damage geometry of that variant.

In Blender, `extras` come from a node's **Custom Properties** (Object Properties → Custom
Properties); the glTF exporter writes them through verbatim.

---

## Material requirements

*(Enforced by `validate-mesh`)*

- All materials must use **PBR metallic-roughness** (`pbrMetallicRoughness` in the glTF JSON)
- No embedded image data — all textures must be external URI references pointing to `.ktx2` files
- Material names must follow the same lowercase-underscore convention as node names
- Opaque materials are rendered **single-sided** (back faces culled), so winding/normals must be
  correct — see [Coordinate system and winding](#coordinate-system-and-winding). Use a
  `KHR_materials` alpha-blend material only for genuinely double-sided surfaces (canopy glass).

Known glTF extensions (`KHR_materials_unlit`, `KHR_materials_emissive_strength`, etc.) produce
a validation **warning** — the renderer may or may not support them. Unknown extensions produce
a validation **error**.

---

## Texture references

All material textures must reference external `.ktx2` files, not embedded PNG data. In Blender,
export textures to separate files then process them with `tex-compress` before referencing them
in the glTF material. The URI in the glTF JSON should be a relative path to the `.ktx2` file:

```json
"baseColorTexture": {
    "index": 0
},
...
"images": [
    { "uri": "../../textures/fa18c_diffuse.ktx2" }
]
```

The URI points at the built `.ktx2` under the pack-level `textures/` directory (two levels up from
`aircraft/<id>/`); the `.ktx2` itself is a release artifact, built by `tex-compress` from the
committed PNG masters — see [`docs/modding/textures.md`](textures.md) for the full pipeline.

---

## Animation channels

*(Enforced by `validate-mesh`)*

**The engine owns named, normalized channels. Your model bakes keyframed node-TRS clips. The runtime
SCRUBS the clip at `t = value × duration` — it never "plays" it.**

That is the contract every established sim implements (DCS numbered draw arguments, X-Plane's named
datarefs, MSFS's simvar → keyframe mapping, BMS's numbered DOF nodes), and it has one consequence
worth stating plainly: **retraction is scrubbing `gear` toward 0.** There is no `gear_retract` clip.
A second clip would be duplicate state to keep in sync, and it matches no shipping sim.

### The channel registry

One clip per channel. **The clip's name IS the channel name.** A clip may target any number of nodes.

| Clip | Semantics | Range |
|---|---|---|
| `gear` | 0 = up, doors closed; 1 = down-locked. One clip sequences struts, doors and linkages | 0..1 |
| `flaps` | 0 = clean, 1 = full. Slats may live in the same clip | 0..1 |
| `speedbrake` | 0 = stowed, 1 = deployed | 0..1 |
| `hook` | 0 = stowed, 1 = down | 0..1 |
| `canopy` | 0 = closed, 1 = open | 0..1 |
| `sweep` | 0 = `[wing_sweep] min_deg`, 1 = `max_deg` | 0..1 |
| `tvc_pitch` | −1 = `[aero.tvc] min_angle_deg`, +1 = `max_angle_deg` | −1..+1 |
| `tvc_yaw` | reserved (the sim is pitch-only today) | −1..+1 |
| `elevator` | −1 = full nose-down, +1 = full nose-up | −1..+1 |
| `aileron` | +1 = right-roll command; one clip animates both surfaces | −1..+1 |
| `rudder` | +1 = right yaw | −1..+1 |
| `prop_spin` (also `rotor_spin`, `wheel_spin`) | **Looping**: the playhead advances at `rate × (1/duration)` rev/s | rate |
| `bay` | 0 = closed, 1 = open | 0..1 |
| `gear_compress_nose` / `_left` / `_right` | 0 = extended, 1 = bottomed. Oleo compression, derived client-side | 0..1 |

Gear *compression* is a separate channel from gear *deploy*, as it is in every sim that models both.

### Authoring rules

1. **Scrub mapping.** Unsigned channels: `t = value × duration`. Signed channels:
   `t = (value + 1) / 2 × duration` — the clip's **start** is full negative deflection, its
   **midpoint** is neutral, its **end** is full positive.
2. **Asymmetric control authority** (`max_elevator_deg` ≠ `max_elevator_neg_deg`) is expressed by
   authoring **asymmetric endpoints**, never by rescaling the parameter. Rescaling would make the
   neutral position depend on the flight model.
3. **Transit timing lives in the simulation, never in the clip.** Clip duration is arbitrary — the
   runtime rescales it. Actuator rate limiting is server-side, from
   [`[articulation]`](flight-model.md#articulation--actuator-transit-times-optional). The one
   exception is a spin clip, whose duration is one revolution at rate 1.0.
4. **A node's rest local TRS must equal its pose at the neutral keyframe** (`t = 0` for an unsigned
   channel, the clip midpoint for a signed one). `validate-mesh` errors on a mismatch: this is the
   most likely authoring mistake, and it renders the aircraft subtly wrong before anything is even
   commanded.
5. **LINEAR and STEP are required; CUBICSPLINE is accepted.** Animating morph-target `weights`, or
   shipping a mesh with a **skin**, is a validation error — rigid mechanical parts are plain animated
   nodes, never joint deformation.
6. **Articulated geometry is authored in node-local space, with the node origin on the hinge line.**
7. **A node may be driven by at most one scrubbed clip** (plus optionally one `gear_compress_*`
   clip — deploy and compression compose only because compression animates the inner strut/oleo
   nodes, disjoint from the doors and linkages the deploy clip drives).
8. A `_b` damage node **inherits its base node's pose**; animating one directly is a warning.
9. **Marker empties are legal.** A node with no mesh (a `camera_anchor`, a hardpoint or hinge marker)
   and arbitrary glTF `extras` are forward-compatible metadata, not errors.
10. **A mesh with zero animations stays valid, forever.** `f5e.glb` as shipped is the compatibility
    baseline.

### Blender recipe

One **action** per channel, pushed to its own single-strip **NLA track named for the channel**, then:

```python
bpy.ops.export_scene.gltf(filepath="f5e.glb", export_animation_mode='NLA_TRACKS')
```

`NLA_TRACKS` mode is what makes each track export as a separately named glTF animation. In the
default mode Blender merges everything into one clip, and none of it will bind.

---

## File size

A `.glb` that exceeds 50 MB is almost certainly embedding texture data rather than referencing
external `.ktx2` files. `validate-mesh` warns on files above this threshold. Aircraft models
without embedded textures are typically 1–15 MB.

---

## Validation

Run `validate-mesh <file.glb>` before committing. The tool automatically discovers and
validates LOD sibling files in the same directory. All errors are reported in a single pass.

Exit codes: 0 = valid, 1 = validation failure, 2 = bad arguments.

Schema source: this document. For a complete list of content pack asset formats see
[`docs/modding/formats.md`](formats.md).

---

## Procedural placeholder generation

`tools/blender_gen.py` generates a parametric fighter aircraft `.glb` set using
Blender's headless Python API. It is intended for development placeholders and
modding examples, not as a substitute for hand-authored art.

**Requirements:** Blender 4.0 or later.

### Invocation

```bash
# Linux
blender --background --python tools/blender_gen.py -- \
    --id fa18c --output-dir assets/aircraft/fa18c/

# macOS
/Applications/Blender.app/Contents/MacOS/blender --background \
    --python tools/blender_gen.py -- \
    --id fa18c --output-dir assets/aircraft/fa18c/

# Windows (PowerShell)
& "C:\Program Files\Blender Foundation\Blender 4.x\blender.exe" --background `
    --python tools\blender_gen.py -- `
    --id fa18c --output-dir assets\aircraft\fa18c\
```

### Options

| Option | Default | Description |
|---|---|---|
| `--id <name>` | — | Asset ID (required). Must match `^[a-z][a-z0-9_]*$`. |
| `--output-dir <path>` | — | Directory to write output files (required). |
| `--wing-style delta\|swept\|straight` | `swept` | Wing planform: swept (generic 3rd-gen), delta (Mirage/F-16 style), straight (subsonic). |
| `--length <m>` | `15.0` | Fuselage length in metres. |
| `--lod` | off | Also export `_lod0`, `_lod1`, `_lod2` variants at reduced resolution. |
| `--bake-textures` | off | Bake a diffuse PNG and generate a solid-colour ORM PNG via Cycles CPU. |
| `--tex-size <px>` | `1024` | Bake resolution (power-of-two). |
| `--seed <n>` | `42` | RNG seed for the damage-state hull breach pattern. |

### Output files

For `--id fa18c --output-dir assets/aircraft/fa18c/ --bake-textures --lod`:

```
assets/aircraft/fa18c/
    fa18c.glb           clean mesh + fa18c_b damage node; .ktx2 URIs pre-wired
    fa18c_dmg.glb       SceneRenderer damageMeshName target
    fa18c_lod0.glb      ~50 % vertex reduction
    fa18c_lod1.glb      ~25 %
    fa18c_lod2.glb      ~10 %
    fa18c_diffuse.png   baked diffuse → tex-compress --type diffuse
    fa18c_orm.png       ORM (R=AO, G=roughness, B=metallic) → tex-compress --type orm
    fa18c_dmg_diffuse.png
    fa18c_dmg_orm.png
```

All `.glb` files pass `validate-mesh` immediately. The `.ktx2` URIs are pre-wired
in the material JSON so that the engine loads them once `tex-compress` has been
run — no manual glTF editing required.

Normal maps are not generated because the engine's built-in flat tangent-space
default (`{128, 128, 255}`) is sufficient for placeholder meshes.

### Texture pipeline after generation

```bash
tex-compress --type diffuse assets/aircraft/fa18c/fa18c_diffuse.png
tex-compress --type orm     assets/aircraft/fa18c/fa18c_orm.png
tex-compress --type diffuse assets/aircraft/fa18c/fa18c_dmg_diffuse.png
tex-compress --type orm     assets/aircraft/fa18c/fa18c_dmg_orm.png
```

See [`docs/modding/textures.md`](textures.md) for the full texture pipeline.
