# Texture Authoring Guide

This guide covers the texture pipeline for fl-base-pack content: how to author source textures,
choose the right compression format, and produce GPU-ready KTX2 files using `tex-compress`.

---

## Overview

The engine's GPU-ready texture format is **KTX2** with BC block compression and a full mipmap
chain. Source assets are **PNG** files authored in Blender, Substance Painter, or any raster
editor. The `tex-compress` tool converts PNG → KTX2 and is run once at pack build time; the
resulting `.ktx2` files are what gets committed to fl-base-pack.

Do not commit source `.png` files — they are large and reproducible from the same source art.

---

## Naming conventions

Lowercase snake_case, matching the mesh asset ID:

```
fa18c_diffuse.png       →  fa18c_diffuse.ktx2
fa18c_normal.png        →  fa18c_normal.ktx2
fa18c_orm.png           →  fa18c_orm.ktx2
fa18c_emissive.png      →  fa18c_emissive.ktx2
```

Place texture files alongside the mesh in the aircraft subdirectory:

```
aircraft/fa18c/
    fa18c.glb
    fa18c.toml
    fa18c_diffuse.ktx2
    fa18c_normal.ktx2
    fa18c_orm.ktx2
    fa18c_emissive.ktx2
```

---

## Resolution

All textures must be **power-of-two** in both dimensions.

| Asset type | Recommended size |
|---|---|
| Aircraft skin (diffuse, normal, ORM) | 2048×2048 or 4096×4096 |
| Cockpit instruments | 1024×1024 |
| Weapon textures | 512×512 |
| Terrain tile | 2048×2048 |

Non-power-of-two textures will fail to generate a complete mipmap chain and will produce
artefacts at distance.

---

## Channel layout per texture type

| Type | Channels | Export from Blender |
|---|---|---|
| **Diffuse / albedo** (opaque) | RGB | Standard color export; no alpha |
| **Diffuse / albedo** (alpha) | RGBA | Enable alpha in export; canopy glass, decals |
| **Normal map** | RG (tangent-space) | Use Blender's normal bake; B channel is reconstructed by the shader from RG — do not invert Y |
| **ORM** (packed) | RGB | R = Ambient Occlusion, G = Roughness, B = Metallic; bake each channel separately and combine |
| **Emissive** | RGB | Cockpit glow, engine exhaust, instrument light |

---

## Format selection

Use `--type` to select the compression preset, or `--format` to override explicitly.

| `--type` | Default format | Reason |
|---|---|---|
| `diffuse` (opaque) | BC1 | Smallest size; no alpha overhead |
| `diffuse` (alpha) | BC3 | Preserves alpha channel; use for canopy glass, decals |
| `normal` | BC7 | Highest quality; preserves RG precision without BC5's two-channel restriction |
| `orm` | BC7 | Packed three-channel data; BC7 avoids cross-channel compression artefacts |
| `emissive` | BC7 | Preserves HDR-range colour values accurately |

When in doubt, use **BC7** — it is the best-quality format and the size penalty is modest on
modern hardware (roughly 2× BC1 for the same texture). Use BC1/BC3 only where storage is
constrained (weapon textures, terrain detail maps).

---

## `tex-compress` usage

```bash
# Basic usage — output defaults to same path with .ktx2 extension
tex-compress --type diffuse  fa18c_diffuse.png
tex-compress --type normal   fa18c_normal.png
tex-compress --type orm      fa18c_orm.png
tex-compress --type emissive fa18c_emissive.png

# Opaque diffuse with explicit format
tex-compress --format bc1 fa18c_diffuse.png

# Diffuse with alpha (canopy)
tex-compress --type diffuse --format bc3 fa18c_canopy.png

# Specify output path explicitly
tex-compress --type diffuse fa18c_diffuse.png aircraft/fa18c/fa18c_diffuse.ktx2

# Disable mipmap generation (UI textures only)
tex-compress --type diffuse --no-mipmaps ui_crosshair.png

# Batch convert all PNGs in a directory (bash)
for f in aircraft/fa18c/*.png; do
    tex-compress --type diffuse "$f"
done

# Windows toktx not in PATH — specify full path
tex-compress --toktx "C:\VulkanSDK\1.3.290.0\Bin\toktx.exe" --type diffuse fa18c_diffuse.png
```

### Flags

| Flag | Default | Description |
|---|---|---|
| `--type diffuse\|normal\|orm\|emissive` | — | Selects compression preset (see table above) |
| `--format bc1\|bc3\|bc7` | BC7 | Override format explicitly, ignoring `--type` default |
| `--no-mipmaps` | off | Skip mipmap generation (use only for UI textures that must not blur) |
| `--toktx <path>` | `toktx` | Path to the `toktx` binary; defaults to `toktx` in PATH |

Exit codes: 0 = success, 1 = conversion failure, 2 = bad arguments.

---

## Prerequisites

`tex-compress` delegates to the Khronos `toktx` CLI tool. Install it before running the
pipeline:

| Platform | Installation |
|---|---|
| **Ubuntu / Debian** | `sudo apt-get install ktx-tools` |
| **macOS** | `brew install ktx-tools` |
| **Windows** | Included with the LunarG Vulkan SDK (already required for engine builds) — `toktx.exe` is in `%VULKAN_SDK%\Bin\` |

If `toktx` is not found in PATH, `tex-compress --help` will print a clear error. Use
`--toktx <path>` to specify the binary explicitly on Windows systems where the Vulkan SDK
directory is not in PATH.

---

## Workflow summary

1. Author source art in Blender / Substance Painter
2. Export each map as PNG (see channel layout table above)
3. Run `tex-compress` to produce `.ktx2` files
4. Commit `.ktx2` files to fl-base-pack
5. Verify with `validate-mesh` that the `.glb` references `.ktx2` URIs (not embedded PNG data)

---

## Known limitations

- BC1 and BC3 do not support HDR values; use BC7 for emissive textures with bright glow effects
- Normal maps stored as BC7 use 4 bytes/texel vs BC5's 2 bytes/texel; the quality gain outweighs
  the cost at typical aircraft texture resolutions
- `tex-compress` cannot auto-detect whether a diffuse texture has a meaningful alpha channel;
  use `--format bc3` explicitly when alpha is needed

For glTF material setup and node naming conventions see [`docs/modding/3d-models.md`](3d-models.md).
