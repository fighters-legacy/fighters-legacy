# Renderer Pipeline & Quality Tiers

> **Status:** initial stub. This document is being filled out under
> [#454](https://github.com/fighters-legacy/fighters-legacy/issues/454); individual sections are
> expanded by the renderer follow-ups as they land (see the per-section issue links). For the
> authoritative, pass-by-pass implementation detail today, see the renderer section of
> [`CLAUDE.md`](../CLAUDE.md); for the high-level layering, see
> [`docs/architecture.md`](architecture.md); for upstream technique citations, see
> [`docs/references.md`](references.md).

The Vulkan backend (`platform/vulkan/VkRenderer`) uses Vulkan 1.3 dynamic rendering (no
`VkRenderPass`/`VkFramebuffer`). World positions are double precision and rendered
camera-relative; the GPU works in float32 against a per-frame `worldOrigin`.

## Per-frame pass order

1. **Particle compute** — `particle_sim.comp` advances the GPU particle pool.
2. **Shadow** — up to 4 PSSM cascades into a depth array (forward-Z).
3. **Forward (opaque)** — Cook-Torrance PBR + CSM. Writes **two** colour attachments: HDR
   (`R16G16B16A16_SFLOAT`) and a world-space **normal G-buffer** (octahedral RGBA16F). The
   terrain path blends grass/dirt/rock/snow by altitude+slope with world-space detail noise and a
   detail normal; distant geometry uses exponential fog (Procedural sky) or analytic aerial
   perspective (Atmospheric sky).
4. **GTAO compute** — `gtao.comp` (full-res, horizon-based) reconstructs world position from depth,
   reads the normal G-buffer, writes AO to `m_aoImage`. Gated by the Ambient Occlusion setting.
5. **Sky + particles** — procedural or analytic-atmospheric sky (`sky.frag`, `SkyUBO`) then particle
   billboards.
6. **Transparent** — alpha-blended items, back-to-front.
7. **Bloom** — bright-pass + separable Gaussian (half-res).
8. **Tonemap** — Khronos PBR Neutral, composites bloom + ambient occlusion, applies FXAA.

## Quality tiers

Settings live in `engine/config/GraphicsSettings.h` (engine) ↔ `platform/RenderTypes.h`
(`RendererSettings`); persisted under `[graphics]` in `config/user.toml`.

| Setting | Values | Notes |
|---|---|---|
| Anti-aliasing | Off / FXAA / TAA | MSAA removed (superseded by TAA). TAA currently selects FXAA-quality AA until the temporal resolve lands ([#443](https://github.com/fighters-legacy/fighters-legacy/issues/443)). |
| Ambient occlusion | Off / Low / High | GTAO; full-res today, half-res + temporal planned ([#448](https://github.com/fighters-legacy/fighters-legacy/issues/448)). |
| Sky quality | Procedural / Atmospheric | Atmospheric = analytic Rayleigh/Mie; precomputed LUTs planned ([#445](https://github.com/fighters-legacy/fighters-legacy/issues/445)). |
| Shadow quality | Off / Low / Medium / High / Ultra | PSSM cascade count + resolution. |
| Particle density | Low / Medium / High / Ultra | GPU particle pool size. |
| Draw distance | Low / Medium / High / Ultra | Entity cull radius (gameplay-relevant; always exposed). |

## Technique notes

Brief per-technique notes; citations in [`docs/references.md`](references.md).

- **PBR materials** — Cook-Torrance (GGX/Smith/Schlick), normal + ORM maps. Default fallback is a
  shaded neutral grey (metallic 0.1, roughness 0.6).
- **Shadows** — PSSM cascaded shadow maps, tight bounding-sphere fit, PCF.
- **Sky & atmosphere** — `SkyUBO`-driven; Procedural FBM sky vs. analytic Rayleigh/HG-Mie
  Atmospheric tier. _Precomputed transmittance/multi-scatter LUTs: [#445]._
- **Aerial perspective** — distance-based Rayleigh+Mie extinction/in-scatter on distant geometry at
  Atmospheric quality.
- **Ambient occlusion** — GTAO; applied in the tonemap pass (`TonemapPush::aoStrength`).
  _Ambient-only application + transparent occlusion: [#449]. Half-res + temporal: [#448]._
- **Terrain & biomes** — procedural biome blend today. _Content-pack biome texture arrays +
  builtin biomes: [#446]; authoring tooling: [#447]._
- **Bloom & tonemap** — bright-pass + Gaussian bloom; Khronos PBR Neutral tonemapper.

### Planned sections (placeholders)

These are tracked and will be written when the feature lands:

- **Anti-aliasing — TAA + CAS** — [#443](https://github.com/fighters-legacy/fighters-legacy/issues/443)
- **Auto-exposure / eye adaptation** — [#444](https://github.com/fighters-legacy/fighters-legacy/issues/444)
- **Temporal upscaling (FSR2 / XeSS)** — [#450](https://github.com/fighters-legacy/fighters-legacy/issues/450)
- **Image-based lighting (IBL)** — [#452](https://github.com/fighters-legacy/fighters-legacy/issues/452)
- **Debug visualization commands** — [#453](https://github.com/fighters-legacy/fighters-legacy/issues/453)
- **GPU budget targets** (per quality tier, RTX 3060 / RX 6600 baseline) and the recommended
  **macOS / MoltenVK** preset — to be measured and filled in.

## Validation

The renderer is not exercised on a GPU in CI (`platform-vulkan` is skipped in the test/asan jobs),
so changes are validated by running the sandbox on real hardware. A headless software-Vulkan
golden-image harness is planned ([#451](https://github.com/fighters-legacy/fighters-legacy/issues/451)).
