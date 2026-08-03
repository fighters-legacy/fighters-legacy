# Renderer Pipeline & Quality Tiers

The authoritative reference for the Vulkan backend: pass order, GPU resource layouts, the
camera-relative invariant, and the quality tiers. For the high-level layering see
[architecture.md](architecture.md); for upstream technique citations see
[references.md](references.md).

`platform/vulkan/VkRenderer` uses **Vulkan 1.3 dynamic rendering** — no `VkRenderPass` and no
`VkFramebuffer` objects.

**Instantiation:** game and tool code must use `createVulkanRenderer()` from
`platform/vulkan/VkRendererFactory.h` — never include `VkRenderer.h` directly. `VkRenderer.h` pulls
in `VkResources.h` → `vk_mem_alloc.h`, which is only on the private include path of
`platform-vulkan`.

## World convention

Right-handed, **Y-up, metres** (matching glTF). The Vulkan clip-space Y-flip is handled in the
projection matrix.

**World positions are `double`/`glm::dvec3`** throughout the engine — `EntityTransform::pos`, the
snapshot origin table, `EntityRenderEntry::position`, `CameraView::worldOrigin`. Snapshot entity
positions are quantized relative to shared grid-cell origins (see
[snapshot quantization](decisions/snapshot-quantization.md)).

### The camera-relative rebase invariant — do not double-subtract

The rebase happens **exactly once**, on the CPU, in double precision.

`SceneRenderer` and `TerrainStreamer::getRenderItems` compute `relPos = worldPos − camera.worldOrigin`
as a `dvec3` and bake it into the `RenderItem`/model transform. The shaders then treat `push.model`
as **already camera-relative** and must not subtract `worldOrigin` again — `mesh.vert` does
`gl_Position = proj·view·(model·pos)` with no further offset. Shadow cascades are likewise built in
camera-relative space (`computeCascades` does not add `worldOrigin`; `mesh.frag` samples shadows with
the camera-relative position directly).

A second subtraction in the vertex shader produced `worldPos − 2·worldOrigin`, pushing the whole
scene `worldOrigin.y` metres below the camera (≈575 m at the sandbox spawn). That is the
"camera floats above the ground / entity tiny and unreachable" class of bug — if you see it, look
here first.

Casting the small relative offset to `vec3` is float32-safe at any scale, including planet scale.

## Per-frame pass order

Per frame: a particle compute dispatch, the shadow + forward-opaque + sky + transparent rendering
scopes, a GTAO compute dispatch between forward and sky, then bloom + tonemap.

### 1. Shadow

Up to `kNumCascades = 4` PSSM cascades rendered into a depth array (`VK_FORMAT_D32_SFLOAT`,
**forward-Z**, depth clear = 1.0). Resolution and cascade count are runtime-configurable via
`ShadowQuality` (`kShadowRes = 2048`, 4 cascades = `ShadowQuality::High` default). Cascade matrices
are computed with a tight bounding-sphere fit; the comparison sampler uses
`VK_COMPARE_OP_LESS_OR_EQUAL` PCF. `ShadowUBO` binds at set 0 binding 2; `sampler2DArrayShadow` at
set 0 binding 3. `ShadowQuality::Off` → `numCascades = 0` with a 1×1 image allocated for descriptor
validity, and the shader returns 1.0 immediately.

> **Shadow passes use forward-Z** (near = 0, far = 1) while scene depth uses **reverse-Z**. These are
> independent depth spaces; do not "fix" one to match the other.

### 2. Particle compute

`particle_sim.comp` is dispatched before the forward pass and advances age/position for up to
`m_maxParticles` slots (runtime-configurable via `ParticleDensity`; `kMaxParticles = 8192` =
`ParticleDensity::High` default; `local_size_x = 64`). New particles are written to a host-visible
spawn staging buffer by the CPU each frame, then `vkCmdCopyBuffer`'d into the device-local pool SSBO
(ring-buffer overwrite). Push constant `{dt, count, gravity, _pad}` (16 bytes). Barrier:
`COMPUTE_WRITE → VERTEX_SHADER_READ` before the forward pass.

### 3. Forward (opaque)

Cook-Torrance PBR (GGX NDF, Smith geometry, Schlick Fresnel) with normal maps + ORM textures (set 1:
base colour / normal / ORM at bindings 0–2).

**Two colour attachments:** [0] HDR offscreen (`VK_FORMAT_R16G16B16A16_SFLOAT`) and [1] a world-space
normal G-buffer (`m_normalImage`, octahedral-encoded RGBA16F, cleared to `oct(+Y)` = (0.5, 1.0)).
`mesh.frag` writes both; single-attachment passes (transparent, sky) drop the location-1 output.

Depth is **reverse-Z** (`VK_FORMAT_D32_SFLOAT`, far = 0.0, compare = `GREATER`). The depth image
carries `SAMPLED` usage so GTAO can read it; `storeOp = STORE`.

**Winding and cull:** `cullMode = BACK`, `frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE` (the shadow and
transparent pipelines match). Combined with the projection Y-flip (`proj[1][1] = -f`), this renders
**standard glTF CCW-from-outside** meshes correctly — the winding cross-product must *agree* with the
outward stored normal, which is what Blender exports. All authored meshes follow this and
`validate-mesh` flags inside-out ones. (A prior `frontFace = CW` double-flipped with the Y-flip and
rendered standard glTF inside-out, masked by an old symmetric single-colour placeholder.)

#### Terrain shading path (`shadingMode == 1`)

Blends grass/dirt/rock/snow by altitude + slope, sampling **biome texture arrays** at set 2
(binding 0 `sampler2DArray` base colour sRGB, binding 1 `sampler2DArray` normal+roughness; array
layer = biome id — 0 grass, 1 dirt, 2 rock, 3 snow) at a fine (~4 m) and coarse (~30 m) detail scale
blended by biome weight, over a finite-differenced detail normal that fades with distance.

The arrays bind via `IRenderer::createTextureArray` + `setTerrainBiomeTextures` — both **non-pure
with defaults**, so mocks and headless builds need no change. `TerrainStreamer::uploadBiomeTextures`
uploads them once at construction: a content pack's `textures/biome_basecolor.ktx2` /
`biome_normalorm.ktx2` (KTX2 array), else the deterministic byte-stable builtin arrays
(`engine/render/BuiltinBiomes.h`, raw-RGBA `TextureUploadDesc::rawLayers` path). Skipped entirely
when the renderer is null (`fl-server`).

Biome **selection** is land-cover-driven: the mesh's `TANGENT` attribute is repurposed for terrain
(which shades from the geometric normal, so a real tangent is unused) to carry `.x` = ESA WorldCover
class (255 = no land cover → elevation/slope fallback), `.y` = normalized elevation (snow line), and
`.zw` = a **spherical-valid detail coordinate** (global face-UV arc length mod 3000 m — seamless
across tiles and LOD, replacing a float-cancelling `worldPos + worldOrigin`). `mesh.vert` passes it
raw as `fragRawTangent`; `mesh.frag` maps it through `biomeWeightsForWorldCover` (a mirror of
`engine/render/BiomeWeights.h`, the single source of truth) with a slope override to rock, a dithered
snow line, and a dark/glossy open-water case. Slope uses the true radial up from
`CameraUBO.planetCenter` (camera-relative, CPU-double-rebased). No new vertex layout and no new
pipeline — the shared forward pipeline reuses the existing 48-byte `Vertex`.

Distance model (`LightUBO.fogParams.w`): 0 = exponential fog (procedural sky), 1 = analytic
Rayleigh + Mie aerial perspective (atmospheric sky).

### 4. GTAO (compute)

`gtao.comp` (local 8×8, full-res) runs after the opaque pass: it reconstructs world position from
depth via `invViewProj`, reads the normal G-buffer, horizon-marches 4 directions × 6 steps, and
writes AO to `m_aoImage` (RGBA16F, AO in `.r`). Gated by `RendererAOMode` — `Off` clears the image to
white. Depth transitions to `DEPTH_READ_ONLY_OPTIMAL` for the compute read and back to
`DEPTH_STENCIL_ATTACHMENT_OPTIMAL` for sky/transparent. AO is applied in the tonemap pass
(`TonemapPush::aoStrength`). Half-res + bilateral upsample + motion-vector temporal accumulation are
a perf follow-on ([#448](https://github.com/fighters-legacy/fighters-legacy/issues/448)).

### 5. Sky + particles

One combined rendering scope: the sky fullscreen triangle (`GREATER_OR_EQUAL`, depth write off)
followed by the particle billboard draw (`GREATER`, depth write off, depth test on). Two instanced
draws (`vkCmdDraw(6, m_maxParticles, 0, 0)`): the additive pipeline (fire/explosion) then the alpha
pipeline (smoke). `particle.vert` reads the particle SSBO and camera UBO from set 0 (bindings 0–1);
the push constant `uint32_t renderAdditive` selects which blend-mode particles each pass emits
(others output degenerate off-screen positions).

The sky pass uses FBM procedural clouds driven by `skyParams.w` (cloud coverage) and `fogParams`
(density / start distance / time of day), plus a procedural but **geographically oriented** star
field (rays looked up in the celestial frame via `worldToCelestial`) and the Moon disc with a
geometric phase (the lit limb faces the Sun), fading in as the Sun sets (`nightFactor`) and with
cloud cover.

### 6. Transparent

Alpha-blended items (`MaterialDesc::alphaBlend = true`) drawn back-to-front through
`m_forwardAlphaPipeline` (depth write **off**, cull `NONE`, `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`).
Sorted in `recordCommandBuffer` by descending squared camera distance.

### 7. Bloom

Enabled when `RendererSettings::bloom = true`. Three half-resolution passes into
`m_bloomImage`/`m_bloomAuxImage`: a bright-pass threshold (`bloom_threshold.frag`), a horizontal
Gaussian blur, and a vertical Gaussian blur. `TonemapPush::bloomStrength` controls the additive blend
weight in the tonemap shader.

### 8. Tonemap

Khronos PBR Neutral + optional bloom composite (binding 1 = bloom buffer) + optional FXAA (5-tap luma
edge detect + 3-sample blur). Fullscreen HDR → swapchain (`B8G8R8A8_SRGB`).

## GPU UBO and push-constant layouts

| Resource | Size | Binding | Notes |
|---|---|---|---|
| `CameraUBO` | 144 B | set 0, binding 0 | Includes `planetCenter` (set by `SceneRenderer` from the streamer's baked radius) for the terrain radial-up slope. |
| `LightUBO` | 64 B | set 0, binding 1 | `fogParams` vec4 = density / startDist / timeOfDay / distance-model. |
| `ShadowUBO` | 288 B | set 0, binding 2 | `numCascades uint32_t` + `_pad[3]` follow `splitDepths`. |
| `SkyUBO` | 240 B | sky set 0, binding 0 | Migrated from a 128-byte push constant. `qualityMode` (0 procedural, 1 atmospheric), `fogParams.w` = camera altitude km, `moonDirection` (xyz + w angular radius), `moonParams` (x illumination, y nightFactor, z celestialValid), `worldToCelestial` (mat4-padded mat3). Room reserved for future transmittance / multi-scatter LUT samplers. |
| `GtaoPush` | 96 B | — | `invViewProj` + params + texel. |
| `TonemapPush` | 32 B | — | texel size + `enableFxaa` + `bloomStrength` + `aoStrength`. |
| `ForwardPushConstants` | 96 B | — | model + baseColorFactor + metallic + roughness + `shadingMode float`: 0 normal PBR, 1 terrain elevation/slope tint, 2 debug per-face colour, 3 procedural runway markings (from the slab's runway-local UV), 4 satellite imagery. Driven by `kRenderFlagTerrain` / `kRenderFlagDebugFaceColor` / `kRenderFlagRunway` / `kRenderFlagTerrainSatellite` on the `RenderItem`. |

**GLM extension headers** are not in `<glm/glm.hpp>` core and must be included explicitly:
`VkRenderer.cpp` needs `<glm/gtc/matrix_transform.hpp>` (`glm::lookAt`) and
`<glm/ext/matrix_clip_space.hpp>` (`glm::orthoZO`); `engine/render/RenderSnapshot.h` and
`engine/entity/EntityManager.cpp` need `<glm/gtc/quaternion.hpp>` (`glm::quat`).

## Texture upload

KTX2 Basis Universal → BC7 (desktop, if `VK_FORMAT_BC7_UNORM_BLOCK` is supported) → ASTC 4×4 (Apple
Silicon, if BC7 is absent) → RGBA32 fallback. All mip levels are uploaded via
`createGpuImageCompressed` using `ktxTexture_GetImageOffset` per mip. sRGB/UNORM views are chosen per
texture semantic (base colour = sRGB, normal/ORM = UNORM). Normal maps use the tangent-space flat
normal default `{128,128,255}`; ORM defaults to all-ones linear white.

**2D texture arrays:** `createGpuImage*` take an `arrayLayers` parameter (a
`VK_IMAGE_VIEW_TYPE_2D_ARRAY` view + per-mip×layer copy regions when > 1). `createTextureArray`
handles both a KTX2 with `numLayers > 1` and the raw-RGBA layer-major path
(`TextureUploadDesc::rawLayers`).

## Pipeline cache

The `VkPipelineCache` is loaded from `pipeline_cache.bin` in the user pref directory at renderer init
(`createPipelineCache`) and written back at shutdown (`savePipelineCache`), feeding every
`vkCreate*Pipelines` call — this cuts pipeline build time on repeat launches. There is no UI, and a
missing or mismatched blob is ignored (Vulkan validates the header/UUID).

## Shader discovery and packaging

`VkRenderer::resolveShaderDir()` tries `SDL_GetBasePath()` + `"shaders/"` first, then the macOS
`.app` bundle path, then the build-tree `FL_SHADER_DIR` fallback.

Release packaging is `cmake --install --component runtime|tools`: `install(TARGETS)` owns the set,
replacing two hand-maintained per-platform lists that had already drifted (and shipped neither
`validate-weapon`/`validate-sensor` nor **`fl-server`**, which the game spawns for single-player — so
every release was unplayable offline). **`COMPONENT` is load-bearing**: the FetchContent dependencies
are not `EXCLUDE_FROM_ALL`, so an unscoped `cmake --install` dumps their headers, libraries and
cmake-configs into the archive. `ci.yml` asserts the install set on every PR.

## HUD and overlay layers

The game submits **four independent overlay layers** each frame via `IRenderer`:

1. **Cockpit HUD** (`fl::IHud*` → the builtin `FlightHud`) — `submitOverlayElements(activeHud->elements())`,
   Cockpit camera mode only. Per-aircraft; `activeHud` is swapped for a content-pack implementation
   when one is provided.
2. **Windshield rain/snow** (`fl::WindshieldRain`) — Cockpit mode only.
3. **Server notices** (`ServerNotice`) — any camera mode; set by `ClientNetEventHandler` on
   `MsgServerNotice`.
4. **Game console** (`GameConsole`) — `setConsoleElements(...)`, any game state.

`submitOverlayElements` **appends** and may be called several times per frame; `setConsoleElements`
takes a non-owning span. Both are cleared by `endFrame`.

`HudElement` (`platform/RenderTypes.h`) is a 2D draw command — Text/Line/Rect at normalized 0–1
screen positions with RGBA colour. Text `x` is an **anchor** interpreted per the `HudAlign` field
(Left/Center/Right, default Left), resolved renderer-side from the string's glyph count; Line and
Rect ignore `align`. `hudAlignOffsetPx(align, widthPx)` is `0` / `−w/2` / `−w`, and text width is
`codepoints × kHudGlyphWidthPx × scale`.

**The HUD font is GNU Unifont 8×16** (`platform/vulkan/UnifontBitmap.h`), covering the full Unicode
BMP (U+0000–U+FFFF). Atlas layout: 512 glyph columns × 128 glyph rows (4096×2048 px `R8_UNORM`);
codepoint **U+FFFF is reserved as a solid-white block** for `Rect`/`Line` fills. The header is
generated by `tools/gen_unifont_header.py` — do not edit it by hand. `HudElement::text` must be valid
UTF-8; non-BMP codepoints (U+10000+) render as U+FFFD (`platform/Utf8Decode.h`).

## Sim → render path (engine side)

Everything above is the Vulkan backend. This is how a simulated world reaches it. All of it lives in
`engine/render/`, and `engine-render` is an **unconditional** CMake library with no Vulkan
dependency — it builds in CI with no GPU.

### `SimRenderBridge` — the lock-free triple buffer

Ships a per-tick entity snapshot from the sim thread to the render thread. Three `RenderSnapshot`
slots rotate: one owned by the sim, one in the atomic spare, one held by the render thread.
`publish()` moves the completed snapshot into the spare (release fence); `tryAdvance()` atomically
swaps the render slot with the spare when a newer tick is available (acq_rel fence). The three slot
indices are always a distinct permutation of {0,1,2}.

- `publishExternal(RenderSnapshot)` is the **main-thread-only** variant for network-client mode,
  where there is no concurrent sim thread. `ClientNetEventHandler` calls it after parsing a
  `WorldSnapshot`; so does `ReplayPlayer` for playback — which is what makes "the renderer cannot
  tell a replay from a live session" true rather than aspirational.
- In server-client mode, do **not** call `EntityManager::setRenderBridge()`. `WorldBroadcaster` owns
  serialisation; the bridge is populated from network packets instead.
- `reset()` zeroes all three slots and clears `hasSnapshot()` — call it between sessions so stale
  data is never displayed.

`EntityRenderEntry` (`engine/render/RenderSnapshot.h`) is POD only, with no `engine-entity` headers
(raw `uint32_t`/`uint8_t` to avoid a circular dependency): entity idx/gen, typeIndex, factionIndex,
`glm::dvec3` position, orientation quat, velocity (sub-tick extrapolation), damage level,
playerOwned, throttle, fuel %, afterburner, `engineFailFlags`, and body-frame `omega` (decoded only
on the receiving peer's own record; `ClientPrediction` seeds the local integrator from it at
reconciliation).

### `SceneRenderer` — snapshot → `FrameScene`

Converts the per-tick snapshot into a `FrameScene` and calls `IRenderer::setScene` each frame. All
work is main-thread. `renderFrame(alpha, camera, env, extraEmitters = {})` calls `tryAdvance()`
internally — callers must **not** also call `renderBridge.tryAdvance()`.

- **`MeshNameResolver`** — a `std::function` injected at construction, to avoid a circular CMake
  dependency between `engine-render` and `engine-entity`. It returns a `ResolvedMesh`
  (`meshName`, `damageMeshName`, `BuiltinShape`).
- **`EffectResolver`** — optional, injected via `setParticleSystem`; called per entity with
  `damageLevel > 0` and returns a particle preset name.
- **Mesh/material cache** — `getOrUploadMesh`/`getOrUploadMaterial` call `IRenderer::createMesh` /
  `createMaterial` once per unique name.
- **Velocity extrapolation** — `rendered_pos = position + velocity × (alpha × kTickDt)`, where alpha
  is the client-side wall-clock `ClientTickAlpha` and `kTickDt = 1/60 s`.
- **Sort and cull** — opaque items sorted front-to-back by squared camera-relative distance to
  minimise overdraw; entities beyond `setDrawDistance(km)` are skipped before `RenderItem`
  construction.
- **Cockpit mode** hides the ownship via `setHiddenEntity()` (shadow-only, `kRenderFlagShadowOnly`) —
  *not* by back-face culling — and draws `EntityDef::cockpitMesh` locked to the airframe via
  `setCockpitMesh()`, so RMB-look turns the view inside a fixed cockpit. Empty = HUD only.

**Builtin placeholder fallback.** When `meshName` is empty or a pack mesh upload fails, the entity
renders as its `BuiltinShape` silhouette — `{Unknown, AirVehicle, Missile, Bomb, Rocket,
GroundVehicle, NavalVessel, Structure}` — with slumped wreck variants for the persistent categories
at `damageLevel > 0` (projectiles despawn on death, so they have no wreck). When the resolver returns
**false** (typeIndex not in the registry) the entity renders the **Unknown error beacon**, a spiky
jack: *a bug state must not look like a plausible aircraft.* Builtin entity items get
`kRenderFlagDebugFaceColor` in debug builds so the forward shader colours faces by normal, making
orientation unambiguous. Geometry lives in `engine/render/BuiltinGeometry.h`, generated by
`tools/gen_builtin_glb.py` (standard CCW-from-outside winding, single-primitive glbs — `VkResources`
reads only `meshes[0].primitives[0]`); CI regenerates and `validate-mesh`-checks the whole exported
set via a glob.

### `ParticleSystem` — CPU emission, GPU simulation

`engine/render/ParticleSystem` manages per-frame emitter emission on the CPU; `VkRenderer` handles
GPU simulation and rendering (passes 2 and 5 above).

`ParticlePreset` carries spawn rate, lifetime, initial speed, start/end colour and size, an additive
flag, `emitDirection` (default `{0,1,0}` = upward hemisphere; `{0,-1,0}` for rain/snow) and
`coneHalfAngleDeg` (90° = full hemisphere; rain 20°, snow 80°). `registerPreset(name, preset)` /
`emit(name, worldPos, intensity)` / `reset()` / `emitters()` follow the per-frame
accumulate-then-read pattern.

The GPU pool is `m_maxParticles` slots in a device-local SSBO (`GpuParticle`, 80 bytes). Spawning is
a ring buffer with a **per-emitter float accumulator** (`m_spawnAccum`, indexed by emitter position
in `particleEmitters`, reset when the list count changes) carrying the fractional remainder across
frames — `toSpawn = uint32_t(accum); accum -= float(toSpawn)` — so a preset with `spawnRate < 60/s`
still emits correctly at 60 fps. Buffers are recreated by `recreateParticleResources()` when
`ParticleDensity` changes.

### `CameraController` — a thin pose holder

`setMode(mode)` + `setPose(eye, forward, up)` + `view(aspect, fovY = 60°, near = 0.1)` → `CameraView`.
It does **no** per-mode math: there is a single underlying free-fly camera, and the modes are just
constrained ways of driving `setPose()`, all computed in the game layer's `CameraInput`.

- **Free** (F4, the base camera) — WASD/QE move the eye, mouse (LMB drag) turns the view. Movement is
  frame-rate independent (m/s × frame dt) and the eye is clamped above `TerrainStreamer::heightAt`.
- **Chase** (F2) — behind the tail, following heading, aimed at the entity origin. Not rotatable.
- **Cockpit** (F1) — locked to the entity, eye at the body centre, looking along entity-forward plus
  the RMB look offset.
- **Projection** — infinite reverse-Z perspective built by hand from `f = 1/tan(fovY/2)`:
  `proj[0][0] = f/aspect`, `proj[1][1] = -f` (Vulkan Y-flip), `proj[2][3] = -1`, `proj[3][2] = near`.
  `VkRenderer` reads `proj[3][2]` as the near-plane value for the shadow cascade split.
- **`view()`** sets `cv.worldOrigin = m_eye` and builds `cv.view` with the camera at the relative
  origin (guarding against forward ∥ up). `CameraView::worldOrigin` is a `dvec3`; `VkRenderer` casts
  it to `vec3` only at GPU upload sites.

## Quality tiers

Settings live in `engine/config/GraphicsSettings.h` (engine) ↔ `platform/RenderTypes.h`
(`RendererSettings`); persisted under `[graphics]` in `config/user.toml`. **The ordinals must stay in
sync between the two enums** — `SettingsScreen::applyAndSave()` maps engine-layer to platform-layer
with a `static_cast`.

| Setting | Values | Notes |
|---|---|---|
| Anti-aliasing | Off / FXAA / TAA | MSAA removed (superseded by TAA). TAA currently selects FXAA-quality AA until the temporal resolve lands ([#443](https://github.com/fighters-legacy/fighters-legacy/issues/443)). |
| Ambient occlusion | Off / Low / High | GTAO; full-res today, half-res + temporal planned ([#448](https://github.com/fighters-legacy/fighters-legacy/issues/448)). |
| Sky quality | Procedural / Atmospheric | Atmospheric = analytic Rayleigh/Mie; precomputed LUTs planned ([#445](https://github.com/fighters-legacy/fighters-legacy/issues/445)). |
| Shadow quality | Off / Low / Medium / High / Ultra | PSSM cascade count + resolution. |
| Particle density | Low / Medium / High / Ultra | GPU particle pool size (512 / 2048 / 8192 / 16384). |
| Draw distance | Low / Medium / High / Ultra | Entity cull radius (gameplay-relevant; always exposed). |

## Technique notes

Citations in [references.md](references.md).

- **PBR materials** — Cook-Torrance (GGX/Smith/Schlick), normal + ORM maps. The default fallback is a
  shaded neutral grey (metallic 0.1, roughness 0.6).
- **Shadows** — PSSM cascaded shadow maps, tight bounding-sphere fit, PCF.
- **Sky & atmosphere** — `SkyUBO`-driven; procedural FBM sky vs. analytic Rayleigh/HG-Mie atmospheric
  tier. _Precomputed transmittance/multi-scatter LUTs: [#445]._
- **Aerial perspective** — distance-based Rayleigh+Mie extinction/in-scatter on distant geometry at
  Atmospheric quality.
- **Ambient occlusion** — GTAO; applied in the tonemap pass. _Ambient-only application + transparent
  occlusion: [#449]. Half-res + temporal: [#448]._
- **Terrain & biomes** — see the terrain shading path above. _Authoring tooling: [#447]._
- **Bloom & tonemap** — bright-pass + Gaussian bloom; Khronos PBR Neutral tonemapper.

### Planned sections (placeholders)

Tracked, and written when the feature lands:

- **Anti-aliasing — TAA + CAS** — [#443](https://github.com/fighters-legacy/fighters-legacy/issues/443)
- **Auto-exposure / eye adaptation** — [#444](https://github.com/fighters-legacy/fighters-legacy/issues/444)
- **Temporal upscaling (FSR2 / XeSS)** — [#450](https://github.com/fighters-legacy/fighters-legacy/issues/450)
- **Image-based lighting (IBL)** — [#452](https://github.com/fighters-legacy/fighters-legacy/issues/452)
- **Debug visualization commands** — [#453](https://github.com/fighters-legacy/fighters-legacy/issues/453)
- **GPU budget targets** (per quality tier, RTX 3060 / RX 6600 baseline) and the recommended
  **macOS / MoltenVK** preset — to be measured and filled in.

## Validation

The renderer is not exercised on a GPU in CI (`platform-vulkan` is skipped in the test and asan
jobs), so changes are validated by running the sandbox on real hardware. `--screenshot <path>`
(`IRenderer::captureScreenshot`, non-pure with a `false` default so mocks need no change) reads the
presented swapchain image back to a PNG and is the reliable in-engine visual-verification path;
`tools/visual_check.{sh,ps1}` builds on it. A headless software-Vulkan golden-image harness is
planned ([#451](https://github.com/fighters-legacy/fighters-legacy/issues/451)).
