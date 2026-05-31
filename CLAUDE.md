# Fighters Legacy — Claude Code Instructions

## Project Overview

GPL v3 general-purpose combat flight sim engine, inspired by Jane's Fighters Anthology (1998).
Cross-platform: Windows 10/11, Linux, macOS. Phase 2 (Modern-Particles Engine) is active.

## Architecture

```
engine/         — core: content system, asset manager, IContentPack interface
engine/entity/  — entity/object system: pool, type registry, damage model, EntityManager
engine/net/     — authoritative server sim: WorldBroadcaster, GameProtocol wire types
engine/perf/    — performance overlay: PerformanceOverlay
engine/render/  — sim→render bridge + scene submission: RenderSnapshot, SimRenderBridge, SceneRenderer, CameraController, ParticleSystem
platform/       — HAL: Vulkan, SDL3, OpenAL Soft, ENet backends
platform/RenderTypes.h — GPU-agnostic scene types shared across the HAL boundary
server/         — dedicated server binary
server/fl-server/ — fl-server: authoritative headless game server (owns GameLoop + EntityManager)
game/           — fighters-legacy game binary (ENet client connecting to embedded fl-server in single-player)
tools/          — developer utilities; asset pipeline (validate-flight-model, validate-mission, validate-licenses, validate-mesh, tex-compress); net_check (ENet smoke-test); blender_gen.py
tests/          — Catch2 unit tests
```

The engine is fully content-agnostic. It knows nothing about FA or any specific game.
FA support lives in jomkz/fa-content. No FA-specific code belongs in this repo.

`ft-gui` is an old name for `fighters-codex` (a separate project). It has no role here. Phase 4 UI work follows the `IGui` HAL pattern (pure-virtual interface + swappable backends, same as `IRenderer`); no ft-gui references belong in this repo.

### Math library

**GLM** is the shared vector/matrix/quaternion library, linked as an INTERFACE dependency on `platform-hal`. Anything that links `platform-hal` (engine, game, tests) gets GLM automatically. Use `glm::vec3`, `glm::mat4`, `glm::quat`, etc.

### Renderer architecture (Phase 2)

`VkRenderer` (platform/vulkan/) uses Vulkan 1.3 dynamic rendering (`VK_KHR_dynamic_rendering`) — no `VkRenderPass` or `VkFramebuffer` objects. Seven steps per frame (one compute dispatch + six rendering scopes):

1. **Shadow** — `kNumCascades=4` PSSM cascades rendered into a `kShadowRes=2048` 2D depth array (`VK_FORMAT_D32_SFLOAT`, **forward-Z**, depth clear = 1.0). Cascade matrices computed via tight bounding-sphere fit; PCF comparison sampler (`VK_COMPARE_OP_LESS_OR_EQUAL`). `ShadowUBO` bound at set 0, binding 2; `sampler2DArrayShadow` at set 0, binding 3.
2. **Particle compute** — `particle_sim.comp` dispatched before forward pass; advances age/pos for `kMaxParticles=8192` slots (local_size_x=64). New particles written to a host-visible spawn staging buffer by CPU each frame then `vkCmdCopyBuffer`'d into the device-local pool SSBO (ring-buffer overwrite). Push constant `{dt, count, gravity, _pad}` (16 bytes). Barrier: COMPUTE_WRITE → VERTEX_SHADER_READ before forward pass.
3. **Forward (opaque)** — Cook-Torrance PBR (GGX NDF, Smith geometry, Schlick Fresnel) with normal maps + ORM textures (set 1: base color / normal / ORM at bindings 0–2). Geometry into HDR offscreen (`VK_FORMAT_R16G16B16A16_SFLOAT`) with **reverse-Z** depth (`VK_FORMAT_D32_SFLOAT`, far = 0.0, depth clear = 0.0, compare = GREATER). Depth storeOp = STORE (sky + particle + transparent + bloom passes read depth).
4. **Sky + particles** — combined rendering scope: sky fullscreen triangle (GREATER_OR_EQUAL, depth write off) followed by particle billboard draw (GREATER, depth write off, depth test on). Two instanced draws (`vkCmdDraw(6, kMaxParticles, 0, 0)`): additive pipeline (fire/explosion) then alpha pipeline (smoke). `particle.vert` reads particle SSBO and camera UBO from set 0 (bindings 0–1); push constant `uint32_t renderAdditive` selects which blend-mode particles to emit (others output degenerate off-screen positions).
5. **Transparent** — alpha-blended items (materials with `MaterialDesc::alphaBlend=true`) drawn back-to-front using `m_forwardAlphaPipeline` (depth write OFF, cull NONE, SRC_ALPHA/ONE_MINUS_SRC_ALPHA blend). Sorted in `recordCommandBuffer` by descending squared camera distance.
6. **Bloom** — enabled when `RendererSettings::bloom=true`. Three half-resolution passes into `m_bloomImage`/`m_bloomAuxImage`: bright-pass threshold (`bloom_threshold.frag`), horizontal Gaussian blur, vertical Gaussian blur. `TonemapPush::bloomStrength` controls additive blend weight in the tonemap shader.
7. **Tonemap** — Khronos PBR Neutral + optional bloom composite (binding 1 = bloom buffer) + optional FXAA (5-tap luma edge-detect + 3-sample blur). `TonemapPush` (16 bytes): texelSizeX/Y, enableFxaa, bloomStrength. Fullscreen HDR → swapchain (`B8G8R8A8_SRGB`).

**Note:** shadow passes use forward-Z (near=0, far=1); scene depth uses reverse-Z. These are independent depth spaces.

World convention: right-handed, Y-up, meters (matches glTF). Vulkan clip-space Y-flip handled in the projection matrix. **World positions are `double`/`glm::dvec3`** throughout the engine (`EntityTransform::pos`, `MsgEntityEntry::pos`, `EntityRenderEntry::position`, `CameraView::worldOrigin`). Camera-relative rendering subtracts `dvec3 worldOrigin` before GPU upload, casting the small relative offset to `vec3` — float32-safe at any scale including planet-scale.

**Texture upload:** KTX2 Basis Universal → BC7 (desktop, if `VK_FORMAT_BC7_UNORM_BLOCK` supported) → ASTC 4×4 (Apple Silicon, if BC7 absent) → RGBA32 fallback. All mip levels uploaded via `createGpuImageCompressed` using `ktxTexture_GetImageOffset` per mip. sRGB/UNORM views chosen per texture semantic (base color = sRGB, normal/ORM = UNORM). Normal maps use tangent-space flat normal default `{128,128,255}`; ORM defaults to all-ones linear white.

Runtime shader discovery: `VkRenderer::resolveShaderDir()` tries `SDL_GetBasePath()` + `"shaders/"` first, then macOS `.app` bundle path, then the build-tree `FL_SHADER_DIR` fallback. Release packages must stage `*.spv` into `dist/shaders/` (see `release.yml`).

**Renderer instantiation:** game and tool code must use `createVulkanRenderer()` from `platform/vulkan/VkRendererFactory.h` — never include `VkRenderer.h` directly. `VkRenderer.h` pulls in `VkResources.h` → `vk_mem_alloc.h`, which is only on the private include path of `platform-vulkan`.

**GLM extension headers:** `VkRenderer.cpp` requires `<glm/gtc/matrix_transform.hpp>` (for `glm::lookAt`) and `<glm/ext/matrix_clip_space.hpp>` (for `glm::orthoZO`). `engine/render/RenderSnapshot.h` and `engine/entity/EntityManager.cpp` require `<glm/gtc/quaternion.hpp>` (for `glm::quat`). These are not in `<glm/glm.hpp>` core — always include them explicitly.

### Server-client architecture (Phase 2, feat/sandbox-server-client)

Single-player runs an **embedded server** inside the game binary: one `ENetNetwork` bound on `127.0.0.1:4778` + `GameLoop` + `EntityManager` + `WorldBroadcaster` run on a dedicated sim thread. The game's main thread connects as an ENet client, receives `MsgWorldSnapshot` packets, and feeds them into `SimRenderBridge::publishExternal()` for rendering. `fl-server` uses the identical server stack for dedicated multi-player.

- **`engine/net/GameProtocol.h`**: packed wire types (`#pragma pack(1)`). Always parse with `std::memcpy` — never cast raw packet pointers; `uint64_t tickIndex` is at wire offset 4 (misaligned). Wire quaternion order: `x, y, z, w` matching `EntityTransform::quat`.
- **`engine/net/WorldBroadcaster`**: `ISimUpdate` + `INetworkEventHandler`. Wraps `EntityManager::onTick`, serialises all live entities into `MsgWorldSnapshot`, broadcasts unreliably each tick. Sends `MsgConnectAck` + `MsgEntityTypeDef[]` on connect (reliable).
- **`ClientNetEventHandler`** (in `game/fighters-legacy/main.cpp`): parses `ConnectAck` and `WorldSnapshot`; calls `bridge.publishExternal()`.
- **ENet threading rule**: server `ENetHost*` is owned exclusively by the sim thread (via `WorldBroadcaster::onTick → net->service(0)`); client `ENetHost*` by the main thread. Never cross-thread.
- **Single-player alpha**: `serverLoop.shellTick()` (not `gameLoop.shellTick()`) drives render interpolation — the embedded `GameLoop` IS the sim.

### Sim→render bridge (Phase 2, PR 4)

`engine/render/SimRenderBridge` is a **lock-free triple-buffer** that ships a per-tick entity snapshot from the sim thread to the render thread. Three `RenderSnapshot` slots rotate: one owned by the sim, one in the atomic spare, one held by the render thread. `publish()` moves the completed snapshot into the spare (release fence); `tryAdvance()` atomically swaps the render slot with the spare when a newer tick is available (acq_rel fence). All three slot indices are always a distinct permutation of {0,1,2}.

- `EntityRenderEntry`: entityIdx, entityGen, typeIndex, position (**glm::dvec3** — double-precision world position), orientation (glm::quat, w-first constructor), velocity (glm::vec3 for sub-tick extrapolation), damageLevel (uint8_t), playerOwned.
- **`publishExternal(RenderSnapshot)`** — main-thread-only variant for network-client mode (no concurrent sim thread). Used by `ClientNetEventHandler` after parsing a `WorldSnapshot` packet.
- In server-client mode, do NOT call `EntityManager::setRenderBridge()` — `WorldBroadcaster` owns serialisation; the render bridge is populated from network packets instead.
- `engine-render` CMake library is **unconditional** (no Vulkan dep) — builds in CI without a GPU. `engine-entity` privately links `engine-render`; `engine-render` privately links `engine-content`. Any binary/test that links `engine-entity` gets both resolved automatically.

### Scene renderer (Phase 2, PR 5)

`engine/render/SceneRenderer` converts the per-tick entity snapshot into a `FrameScene` and calls `IRenderer::setScene` each frame. All work is main-thread.

- **MeshNameResolver**: `std::function<bool(uint32_t typeIndex, std::string& mesh, std::string& dmg)>` — injected at construction to avoid a circular CMake dep between `engine-render` and `engine-entity`. In `main.cpp` the lambda captures `EntityTypeRegistry&`.
- **EffectResolver**: `std::function<std::string(uint32_t typeIndex, uint8_t damageLevel)>` — optional; injected via `setParticleSystem(ParticleSystem*, EffectResolver)`. Called for each entity with `damageLevel > 0`; returns the particle preset name (e.g. `"explosion"`) or empty string. In `main.cpp` reads `EntityDef::damage` through the registry.
- **Mesh/material cache**: `getOrUploadMesh` / `getOrUploadMaterial` call `IRenderer::createMesh` / `createMaterial` once per unique mesh name; subsequent frames use the cached handle.
- **Camera-relative rendering**: `glm::vec3 relPos = glm::vec3(dvec3(entry.position) - camera.worldOrigin)` — subtracts two `dvec3` values and casts the small result to `vec3` for GPU upload. Safe at planet scale.
- **Velocity extrapolation**: `rendered_pos = entry.position + entry.velocity * (alpha * kTickDt)` where `alpha = GameLoop::shellTick()` and `kTickDt = 1/60 s`.
- **Damage variant**: if `entry.damageLevel > 0` and `EntityDef::classicDamageMesh` is non-empty, the damage mesh is loaded instead; `kRenderFlagDamaged` is set on the `RenderItem`.
- **Sort**: opaque items sorted front-to-back by squared camera-relative distance to minimise overdraw.
- **Draw-distance cull**: `setDrawDistance(float km)` configures the entity cull radius; entities beyond it are skipped before RenderItem construction. Wired from `RendererSettings::drawDistanceKm` in `main.cpp`.
- **Builtin palette fallback**: when `MeshNameResolver` returns false, `meshName` is empty, or mesh upload fails, the entity is rendered with a builtin tetrahedron (~10 m, +X forward) from a 6-color palette cycled by `entityIdx` (0–2 opaque, 3–5 glass/transparent). Palette and meshes are uploaded once on first `renderFrame` call via `ensureBuiltins()`. `setBuiltinFloor(true)` appends a 4 km flat floor plane as the final opaque item. Both are defined in `engine/render/BuiltinGeometry.h`.
- `SceneRenderer::renderFrame(alpha, camera, env, extraEmitters={})` calls `tryAdvance()` internally; callers must NOT also call `renderBridge.tryAdvance()`. `extraEmitters` is merged with entity-damage effects when a ParticleSystem is set.

### GPU particle system (Phase 2, PR 6)

`engine/render/ParticleSystem` manages per-frame emitter emission on the CPU. `VkRenderer` handles GPU simulation and rendering.

- **ParticlePreset** (`engine/render/ParticleSystem.h`): spawnRate, particleLifetime, initialSpeed, colorStart/End, sizeStart/End, additive flag.
- `ParticleSystem::registerPreset(name, preset)` — register by name. `emit(name, worldPos, intensity=1)` fills a `ParticleEmitterState` from the preset and appends to the per-frame list. `reset()` + `emitters()` follow the per-frame accumulate-then-read pattern.
- **GPU pool**: `kMaxParticles=8192` slots in a device-local SSBO (`GpuParticle` — 80 bytes: pos+age, vel+maxAge, colorStart, colorEnd, sizeStart/sizeEnd/additive/_pad). Ring-buffer spawn: CPU writes up to `kMaxSpawnPerFrame=512` new particles to a per-frame host-visible staging buffer, `vkCmdCopyBuffer` into the pool.
- **Compute**: `particle_sim.comp` (local_size_x=64) decrements age, advances pos by vel×dt, applies gravity. Dead particles (age≤0) written as zero and skipped in vertex shader.
- **Render**: `particle.vert` reads SSBO (set 0 binding 0) + camera UBO (set 0 binding 1); emits a 6-vertex camera-facing quad per instance (2 triangles). `particle.frag` outputs circular soft-edge colour with quadratic alpha falloff. Push constant `uint32_t renderAdditive` selects which particles each draw pass processes.
- **DamageDef.visualEffect hook**: `DamagePenalty::visualEffect` (string in `DamageDef`) is the preset name emitted by `SceneRenderer` via `EffectResolver` when an entity is damaged.

`engine/render/CameraController` produces a `CameraView` each frame.

- **Free mode** (default): spherical orbit around a configurable pivot. `setFreeOrbit(pivot, yaw°, pitch°, distance_m)`. Pivot is `glm::dvec3`.
- **Chase mode**: camera offset `kChaseBack=30 m` behind and `kChaseUp=5 m` above the target entity in entity-local space. `setTarget(worldPos, worldOri)` must be called each frame. `worldPos` is `glm::dvec3`.
- **Projection**: infinite reverse-Z perspective hand-built from `f = 1/tan(fovY/2)`: `proj[0][0]=f/aspect`, `proj[1][1]=-f` (Vulkan Y-flip), `proj[2][3]=-1`, `proj[3][2]=near`. `VkRenderer` reads `proj[3][2]` as the near-plane value for shadow cascade split.
- **`CameraView::worldOrigin`** is `glm::dvec3`. `VkRenderer` casts it to `vec3` only at GPU upload sites (shadow cascade center, particle origin).

### World terrain and theaters (feat/world-terrain)

The engine uses a single continuous **world terrain** rather than per-theater heightmap grids. Theaters are geographic regions (bounding boxes in world coordinates) within this single terrain, not separate maps. Players can fly anywhere in the world in one session; theater boundaries are mission conditions, not engine limits.

**Terrain ID:** `"world"` is the canonical ID. fl-base-pack provides global coverage; theater content packs override individual chunks at higher mod priority via `IContentPack::resolveTerrainChunk()`.

**Chunk format:** 16-bit grayscale PNG, 513×513 pixels, **15,360 m per chunk** (512 intervals × 30 m = native Copernicus GLO-30 resolution — no upsampling needed). Three LOD levels: LOD 0 = 513×513, LOD 1 = 257×257, LOD 2 = 129×129. Chunk path convention: `terrain/<id>/lod<n>/chunk_<x>_<y>.png` (all lowercase, 4-digit zero-padded coordinates).

**LOD ring distances** (for a 15,360 m chunk): LOD 0 within 3×3 chunks (~46 km), LOD 1 within 5×5 (~77 km), LOD 2 within 7×7 (~107 km); evict beyond ~120 km. Maximum ~83 chunks in memory at steady state.

**`IContentPack::resolveTerrainChunk(terrainId, chunkX, chunkY, lod) → optional<string>`**: pure virtual — implemented in #172. `FolderContentPack` probes `terrain/<id>/lod<n>/chunk_<x>_<y>.png` under its mod dir (zero-padded 4-digit coords). `AssetManager::resolveTerrainChunk()` walks the priority stack; first-wins, not cached. `TerrainStreamer` calls this to resolve the path before queuing `IAsyncFilesystem::readFileAsync()`. `MockContentPack` in `test_content_system.cpp` has a configurable `chunkPaths` map (key format: `"terrainId:x:y:lod"`) for priority-stack tests; all other test mocks (`EmptyContentPack`, `NullContentPack`, `MockContentPack` in `test_scene_renderer.cpp`) stub it as `return std::nullopt;`.

**`TerrainStreamer`** (`engine/render/TerrainStreamer.h`, after #173): manages chunk lifecycle. Constructor takes `TerrainManifest`, `AssetManager&`, `IAsyncFilesystem&`, `IRenderer*` (nullable for headless server mode). `update(glm::dvec3 cameraPos)` drives async load/evict. `heightAt(double x, double z) → double` for flight model / collision. `surfaceAt(double x, double z) → uint8_t` for landing detection. PNG decode uses `stbi_load_16_from_memory()` in a dedicated `TerrainChunkDecoder.cpp` TU with `#define STB_IMAGE_STATIC` + `STB_IMAGE_IMPLEMENTATION` — the `STATIC` define prevents ODR conflict with `VkResources.cpp`.

**Terrain rendering:** chunks are submitted as standard `RenderItem`s via `IRenderer::createMesh()`/`createMaterial()` — no new `IRenderer` pure virtuals, no `FrameScene` changes, no `MockRenderer` updates required for Phase 2.

**fl-server** must initialize `FolderContentPack` + `AssetManager` + headless `TerrainStreamer` (null renderer) to serve `heightAt()` for authoritative physics (#174).

**DEM source:** Copernicus GLO-30 (ESA, 30 m global, free). `tools/gen_terrain_chunks.py` (#176) converts GeoTIFF → chunk PNGs at all 3 LOD levels. fl-base-pack `scripts/build_terrain.sh` downloads tiles from AWS Open Data (`s3://copernicus-dem-30m/`) and invokes the tool.

**Wire protocol note:** `MsgEntityEntry::pos[3]` is `double` (struct is 68 bytes, up from 56). Always parse with `std::memcpy`. #142 (authoritative game protocol) depends on #170 landing first.

## Build

```bash
# Linux / macOS
cmake --preset debug && cmake --build --preset debug

# Windows (PowerShell)
cmake --preset debug-msvc && cmake --build --preset debug-msvc

# Run tests
ctest --preset debug --output-on-failure
```

See docs/development.md for prerequisites (Vulkan SDK, SDL3, OpenAL, ENet, Catch2).

## Conventions

- Conventional Commits — scopes: engine / renderer / audio / network / content / i18n / flight / difficulty / entity / ai / mission / game / tools / build / ci / docs
- DCO sign-off required: `git commit -s`
<!-- REUSE-IgnoreStart -->
- SPDX header required on all new .cpp/.h files: `// SPDX-License-Identifier: GPL-3.0-or-later`
- Shader files (`.vert`, `.frag`, `.comp`, `.glsl`) are covered by REUSE.toml — no inline SPDX needed, but **add any new shader extension to REUSE.toml** or the REUSE CI step will fail.
<!-- REUSE-IgnoreEnd -->
- All code must compile on Windows (MSVC 2022), Linux (GCC/Clang), macOS (Apple Clang)
- `CMAKE_COMPILE_WARNING_AS_ERROR=ON` in debug builds — fix all warnings

## Key Files

- `README.md` — project overview and documentation index
- `docs/architecture.md` — engine architecture overview
- `docs/development.md` — build prerequisites per platform
- `GOVERNANCE.md` — decision-making and RFC process
- `CMakePresets.json` — all build presets (debug / release / coverage / asan / msvc variants)
- `platform/RenderTypes.h` — GPU-agnostic POD types: `MeshHandle`, `TextureHandle`, `MaterialHandle`, `CameraView`, `RenderItem`, `FrameScene`, `EnvironmentState`, `ParticleEmitterState`, `FrameStats`; also `RendererVsyncMode` + `RendererSettings`
- `platform/IRenderer.h` — `getFrameStats() const` + `setOverlayLines(span<string_view>)` added; any mock must implement both
- `engine/net/WorldBroadcaster.h` — `ISimUpdate` + `INetworkEventHandler`; wraps EntityManager, broadcasts world state each tick
- `engine/perf/PerformanceOverlay.h` — F3 overlay (Off/Compact/Full); `update(FrameStats, entityCount, simTickMs)` + `lines()` → span for `IRenderer::setOverlayLines`; `OverlayMode` lives in `engine/config/DebugSettings.h`
- `engine/config/DebugSettings.h` — `OverlayMode` enum + `DebugSettings` struct; persisted as `[debug].overlay_mode` in user.toml
- `engine/render/RenderSnapshot.h` — `EntityRenderEntry` + `RenderSnapshot`; POD only, no engine-entity headers (uses raw uint32_t/uint8_t to avoid circular deps)
- `engine/render/SimRenderBridge.h` — lock-free triple-buffer bridge; `publish()` sim-thread-only, `tryAdvance()`/`current()`/`hasSnapshot()` render-thread-only; `publishExternal()` main-thread-only for network-client mode
- `engine/render/SceneRenderer.h` — snapshot→FrameScene bridge; `renderFrame(alpha, camera, env, extraEmitters={})` between beginFrame/endFrame; handles mesh upload/cache, camera-relative transforms, damage variant, front-to-back sort, draw-distance cull; `setParticleSystem(ps, effectResolver)` wires damage-effect emission; `setDrawDistance(km)` sets entity cull distance; `setBuiltinFloor(bool)` enables the 4 km floor plane
- `engine/render/BuiltinGeometry.h` — `builtinTetrahedronGlb()` + `builtinFloorPlaneGlb()`: embedded geometry for the no-content-pack sandbox; generated by `tools/gen_builtin_glb.py`. `builtinWorldTerrainChunks()` and `tools/gen_builtin_terrain.py` are planned — tracked in #188.
- `engine/render/TerrainStreamer.h` — async chunk lifecycle manager; `update(dvec3)`, `heightAt(double,double)→double`, `surfaceAt(double,double)→uint8_t`; issue #173
- `engine/net/GameProtocol.h` — `MsgEntityEntry::pos[3]` is `double` (68 bytes total); always parse with `memcpy`; issue #170
- `engine/render/CameraController.h` — Free-orbit + Chase cameras; `view(aspectRatio)` → `CameraView`; infinite reverse-Z projection with Vulkan Y-flip
- `engine/render/ParticleSystem.h` — `ParticlePreset` + `ParticleSystem`; preset registry, per-frame emit/reset/emitters() accumulator; `DamagePenalty::visualEffect` maps to preset name
- `cmake/dependencies.cmake` — all FetchContent declarations; GLM is unconditional, Vulkan-specific deps are gated on `Vulkan_FOUND`
- `platform/vulkan/VkRendererFactory.h` — thin factory header; only include needed by game/tools to instantiate the renderer
- `tools/blender_gen.py` — headless Blender 4.x parametric aircraft mesh generator; `blender --background --python tools/blender_gen.py -- --id <id> --output-dir <dir> [--wing-style delta|swept|straight] [--lod] [--bake-textures]`; outputs `<id>.glb` (clean + `_b` node), `<id>_dmg.glb`, optional LODs + PNGs; `.ktx2` URIs pre-wired in GLB JSON for tex-compress
