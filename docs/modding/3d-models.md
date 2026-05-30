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
