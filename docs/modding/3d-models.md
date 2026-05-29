# 3D Model Authoring Guide

This guide covers the engine conventions for glTF 2.0 aircraft and unit meshes. Follow it
to ensure your models are accepted by `validate-mesh` and render correctly at runtime.

---

## Overview

The engine loads `.glb` (binary glTF) and `.gltf` (JSON + separate `.bin`) files as raw bytes
via `FolderContentPack`. The renderer parses glTF 2.0 at load time. All conventions below
exist so the renderer can locate expected nodes and textures without heuristics.

Mesh files live in `aircraft/<id>/` inside the content pack directory.

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

## Material requirements

*(Enforced by `validate-mesh`)*

- All materials must use **PBR metallic-roughness** (`pbrMetallicRoughness` in the glTF JSON)
- No embedded image data — all textures must be external URI references pointing to `.ktx2` files
- Material names must follow the same lowercase-underscore convention as node names

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
    { "uri": "../textures/fa18c_diffuse.ktx2" }
]
```

See [`docs/modding/textures.md`](textures.md) for the full texture pipeline workflow.

---

## Animation conventions

Animation names are not validated, but the renderer expects these specific names when present:

| Animation name | Trigger |
|---|---|
| `gear_extend` | Landing gear extending |
| `gear_retract` | Landing gear retracting |
| `prop_spin` | Propeller rotation (turboprops/pistons) |
| `bay_open` | Weapon bay opening |
| `bay_close` | Weapon bay closing |
| `sweep` | Wing sweep (variable-geometry aircraft) |

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
