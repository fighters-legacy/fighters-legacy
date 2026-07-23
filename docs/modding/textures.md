# Texture Authoring Guide

This guide covers the texture pipeline for fl-base-pack content: how to author and commit source
textures, choose the right compression format, and produce the GPU-ready KTX2 files a release ships
using `tex-compress`.

---

## Source vs. artifact — what you commit

The industry pattern is unambiguous, and this project follows it: **PNG masters are the source and
are committed; KTX2 is a build artifact, produced by `tex-compress`, and is NOT committed.**

- **Source, committed** — the PNG masters, under the aircraft's own directory:
  `aircraft/<id>/textures-src/<id>_diffuse.png`, `<id>_orm.png`, `<id>_normal.png`,
  `<id>_emissive.png`. This is "the preferred form of modification" a CC-BY content pack is supposed
  to ship: anyone can repaint from it, and the compression settings can be revisited later
  (ETC1S vs UASTC, per-map tuning) without re-authoring.
- **Artifact, NOT committed** — the `.ktx2` files. A KTX2 is a lossy, block-compressed, transcoded
  output; committing only it loses the source. `.ktx2` is git-ignored, produced by `tex-compress` in
  the pack's release workflow, and shipped in the release archive under `textures/`.

The engine loads `.ktx2` at runtime (with a `.png` fallback — see *Local-dev fallback* below), so a
release archive still contains `textures/<name>.ktx2`; your repository does not.

---

## Naming conventions

Lowercase snake_case, matching the mesh asset ID. Source masters live beside the aircraft; the built
KTX2 lands in the pack-level `textures/` directory (the runtime layout, next section):

```
aircraft/fa18c/textures-src/     ← committed PNG masters (source)
    fa18c_diffuse.png
    fa18c_normal.png
    fa18c_orm.png
    fa18c_emissive.png
    fa18c_uv_layout.png          ← optional UV-layout template for painters

textures/                        ← built KTX2, shipped in the release archive (git-ignored)
    fa18c_diffuse.ktx2
    fa18c_normal.ktx2
    ...
```

(The `_uv_layout.png` painter template is a convention; fl-base-pack does not ship one yet.)

---

## Runtime layout

At runtime the engine resolves `AssetType::Texture` to the pack-level **`textures/`** directory —
`textures/<name>.ktx2`, with a `.png` fallback — **not** to a file beside the `.glb`. A texture placed
next to the mesh will not be found. This is the layout a release archive ships:

```
textures/
    fa18c_diffuse.ktx2
    fa18c_orm.ktx2
aircraft/fa18c/
    fa18c.glb        (references ../../textures/fa18c_diffuse.ktx2)
    fa18c.toml
```

---

## Local-dev fallback (no compressor needed)

You do not need `toktx` installed to iterate. The engine tries `textures/<name>.ktx2` first and
falls back to `textures/<name>.png`, so during development you can drop a PNG straight into
`textures/` and the mesh renders with it — no compression step. Releases ship the `.ktx2`; your
local checkout can use the `.png`. (The `fl-viewer` tool and hot-reload both honour this fallback,
so an author can preview and iterate on textures with nothing but PNGs.)

---

## Texture URI convention

The `.glb` references each texture by a **relative URI that resolves into `textures/`**. The engine
maps that URI back to a texture asset name by taking the path after the last `textures/` segment and
dropping the extension, then loads `textures/<name>.ktx2` (`.png` fallback):

```
../../textures/fa18c_diffuse.ktx2   →  asset "fa18c_diffuse"  →  textures/fa18c_diffuse.ktx2
fa18c_diffuse.ktx2                  →  asset "fa18c_diffuse"   (bare basename also accepted)
```

Author the full `../../textures/<name>.ktx2` form — it is what the shipped meshes use and what
`validate-mesh` expects. A URI whose extension is not `.ktx2` or `.png` will never load and is
flagged by `validate-mesh`. To reference a KTX2/Basis texture the spec-conformant way the mesh
declares `KHR_texture_basisu` in `extensionsUsed`; `validate-mesh` accepts it.

Because `.ktx2` files are build artifacts (produced by `tex-compress`, not committed as sources),
`validate-mesh` reports an unreachable texture URI as a **warning**, not an error — the mesh still
validates when the compressed textures have not been built yet.

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

## Encoding selection — Basis Universal

`tex-compress` produces **Basis Universal** KTX2, not raw block-compressed textures. A Basis KTX2
stores a *transcodable* payload (`vkFormat = VK_FORMAT_UNDEFINED`); at load time the engine transcodes
it to the block format the running GPU wants — **BC7 on desktop, ASTC 4×4 on Apple Silicon**, RGBA32
as a last resort (`ktxTexture2_TranscodeBasis`, `VkResources.cpp`). This is why one committed texture
runs everywhere; it is what `KHR_texture_basisu` exists for. (Raw BCn is a dead end here — `toktx` has
no raw-BCn encoder, so asking for one silently yields an *uncompressed* texture.)

There are two Basis encodings, and the split matters:

| `--type` | Encoding | Why |
|---|---|---|
| `diffuse` (base color / albedo) | **ETC1S** | Small, heavily supercompressed. Fine for colour. |
| `normal` | **UASTC** | ETC1S mangles tangent-space normals — banding shows as lighting artefacts. |
| `orm` | **UASTC** | Packed three-channel data; ETC1S's block palette corrupts the separate channels. |
| `emissive` | **UASTC** | Preserves bright glow values. |

UASTC is larger than ETC1S, so `tex-compress` always zstd-supercompresses it (`--zcmp`) — a UASTC
KTX2 without it is several times the size. Rule of thumb: **base color → ETC1S, everything else →
UASTC.** Override the preset with `--format etc1s|uastc` when you have a reason.

---

## `tex-compress` usage

```bash
# Presets pick the encoding: diffuse -> etc1s, normal/orm/emissive -> uastc.
# Output defaults to the same path with a .ktx2 extension.
tex-compress --type diffuse  fa18c_diffuse.png
tex-compress --type normal   fa18c_normal.png
tex-compress --type orm      fa18c_orm.png
tex-compress --type emissive fa18c_emissive.png

# Force an encoding explicitly (e.g. a high-fidelity base color)
tex-compress --format uastc fa18c_diffuse.png

# Specify output path explicitly (textures live in the pack's textures/ directory)
tex-compress --type diffuse fa18c_diffuse.png textures/fa18c_diffuse.ktx2

# Disable mipmap generation (UI textures only)
tex-compress --type diffuse --no-mipmaps ui_crosshair.png

# Batch convert a mesh's committed masters (bash)
for f in aircraft/fa18c/textures-src/*.png; do
    tex-compress --type diffuse "$f" "textures/$(basename "${f%.png}").ktx2"
done

# Windows toktx not in PATH — specify full path
tex-compress --toktx "C:\VulkanSDK\1.3.290.0\Bin\toktx.exe" --type diffuse fa18c_diffuse.png
```

### Flags

| Flag | Default | Description |
|---|---|---|
| `--type diffuse\|normal\|orm\|emissive` | — | Selects the Basis encoding preset (see table above) |
| `--format etc1s\|uastc` | `uastc` | Override the encoding explicitly, ignoring `--type` |
| `--no-mipmaps` | off | Skip mipmap generation (use only for UI textures that must not blur) |
| `--layers <in…>` | — | 2D-**array** mode: pack N layer-major PNGs into one array KTX2 (see below) |
| `-o, --output <path>` | — | Output KTX2 path (required in `--layers` mode; optional otherwise) |
| `--toktx <path>` | `toktx` | Path to the `toktx` binary; defaults to `toktx` in PATH |

The deprecated `--format bc1\|bc3\|bc7` aliases still parse — they map to `etc1s` (bc1/bc3) or
`uastc` (bc7) with a warning — so old build scripts keep working.

Exit codes: 0 = success, 1 = conversion failure, 2 = bad arguments.

## Texture arrays (biome terrain layers)

The terrain renderer samples its detail textures from a **2D array** — one array for base colour,
one for combined normal+roughness ("normalORM") — where the **array layer index IS the biome id**.
`tex-compress --layers` packs N same-size PNGs (layer-major, requires ≥ 2) into a single array KTX2:

```bash
# Base colour array (sRGB): layer 0 grass, 1 dirt, 2 rock, 3 snow.
tex-compress --type diffuse --layers grass_c.png dirt_c.png rock_c.png snow_c.png \
             -o textures/biome_basecolor.ktx2

# Normal+roughness array (linear): RG = tangent-space normal xy, B = roughness, A = occlusion.
tex-compress --type orm --layers grass_n.png dirt_n.png rock_n.png snow_n.png \
             -o textures/biome_normalorm.ktx2
```

**Layer-order convention (load-bearing — this ordering is ABI):**

| Layer | Biome |
|---|---|
| 0 | grass |
| 1 | dirt  |
| 2 | rock  |
| 3 | snow  |

Place the two arrays at `textures/biome_basecolor.ktx2` and `textures/biome_normalorm.ktx2` in the
pack (they load by texture asset name — `biome_basecolor`/`biome_normalorm` — so they live under the
pack's `textures/` directory like any other texture). When a pack omits them the engine falls back to
a compiled-in procedural biome set, so custom arrays are optional — but if provided they must follow
the layer order above (the shader indexes by biome id, not by name). `--layers` needs `toktx` v4.3 or
newer.

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

1. Author source art in Blender / Substance Painter.
2. Export each map as PNG (see channel layout table above) and **commit it** under
   `aircraft/<id>/textures-src/`.
3. For local testing, drop the PNGs into `textures/` and iterate — the engine's `.png` fallback
   renders them with no compression step (see *Local-dev fallback*).
4. In the pack's **release workflow**, run `tex-compress` over the committed masters to produce
   `.ktx2` into `textures/` (git-ignored), e.g.:

   ```bash
   for src in aircraft/*/textures-src/*.png; do
       out="textures/$(basename "${src%.png}").ktx2"
       # pick --type from the map suffix (diffuse/normal/orm/emissive)
       tex-compress --type diffuse "$src" -o "$out"
   done
   ```

5. Verify with `validate-mesh` that the `.glb` references `../../textures/<name>.ktx2` URIs (not
   embedded PNG data), and with `validate-mod` that the whole pack is consistent.

`.ktx2` files are build outputs of `tex-compress`, reproducible from the committed PNG masters;
treat them as you would any generated artifact — git-ignore them, ship them in the release archive.

---

## Known limitations

- Basis stores LDR only; a bright emissive glow authored in HDR must be tone-mapped into the 8-bit
  PNG master before encoding.
- ETC1S has a single block palette shared across all channels — never use it for a normal map or a
  packed ORM map, only for base color. `--type` already enforces this; only an explicit
  `--format etc1s` on a normal/ORM map defeats it.
- The transcode target is chosen by the running GPU, so the on-disk KTX2 is identical everywhere but
  the sampled block format (BC7 vs ASTC) is not — do not assume byte-identical GPU output across
  platforms when comparing golden images.

For glTF material setup and node naming conventions see [`docs/modding/3d-models.md`](3d-models.md).
