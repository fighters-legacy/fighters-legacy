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
tools/          — developer utilities; asset pipeline (validate-flight-model, validate-mission, validate-licenses, validate-mesh, tex-compress); net_check (ENet smoke-test); blender_gen.py; gen_terrain_chunks.py
tests/          — Catch2 unit tests (C++); pytest unit tests for Python tools (test_gen_terrain_chunks.py)
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

- **`engine/net/GameProtocol.h`**: packed wire types (`#pragma pack(1)`). Always parse with `std::memcpy` — never cast raw packet pointers. `kProtocolVersion = 1` (uint16_t). Seven message types: `MsgHello` (4 bytes, `MsgId::Hello = 0x00`, server→client version handshake), `MsgConnectAck` (12 bytes, includes `assignedEntityIdx/Gen`), `MsgEntityTypeDef` (196 bytes), `MsgWorldSnapshotHeader` (12 bytes, offset 1 = `protocolVersion uint8_t`), `MsgEntityEntry` (68 bytes, double pos), `MsgClientInput` (44 bytes, `MsgId::ClientInput = 0x03`, offsets 2–3 = `protocolVersion uint16_t`), `MsgLanBeacon` (74 bytes, `MsgId::LanBeacon = 0x10`, raw UDP broadcast not sent over ENet). `kGameModeCampaign/Mission/Sandbox` bit constants for `MsgLanBeacon::gameModeFlags`. Wire quaternion order: `x, y, z, w` matching `EntityTransform::quat`. See `docs/network-protocol.md` for field tables.
- **`engine/net/WorldBroadcaster`**: `ISimUpdate` + `INetworkEventHandler`. `onConnect` sends `MsgHello` (reliable) first, then spawns `"builtin:debug-entity"` for the peer, creates a `FlightIntegrator` (via `BuiltinFlightModel::get()`) stored in `m_peerFlightSims`, and stores the entity in `m_peerEntities`, then sends `MsgConnectAck` + `MsgEntityTypeDef[]` (reliable); `onDisconnect` kills the entity and erases both maps. `onReceive` validates `MsgClientInput::protocolVersion == kProtocolVersion` (discards + warns on mismatch) then stores → `PeerInputState` (`m_peerInputs`). `onTick` steps each peer's `FlightIntegrator` from stored inputs (maps `PeerInputState` → `ControlInput`), copies `FlightState` → `EntityState`, then calls `EntityManager::onTick` and serialises all live entities into `MsgWorldSnapshot` (header carries `protocolVersion`) and broadcasts unreliably. `engine-net` privately links `engine-flight`. `getPeerCount() const noexcept` — reads `std::atomic<int> m_activePeerCount` (incremented on connect, decremented on disconnect); safe to call from the main thread for the LAN discovery beacon.
- **`ClientNetEventHandler`** (in `game/fighters-legacy/main.cpp`): parses `MsgHello` (checks `protocolVersion == kProtocolVersion`, calls `net.disconnect()` on mismatch), `ConnectAck` (stores `assignedEntityIdx/Gen`), `WorldSnapshot` (calls `bridge.publishExternal()`). Sends `MsgClientInput` (carries `protocolVersion`) to the embedded server each frame (arrow keys + shift + space).
- **ENet threading rule**: server `ENetHost*` is owned exclusively by the sim thread (via `WorldBroadcaster::onTick → net->service(0)`); client `ENetHost*` by the main thread. Never cross-thread.
- **`GameLoop::enqueueSimCallback(fn)`**: thread-safe lambda queue drained at the top of each sim tick before `ISimUpdate::onTick()`. Use this whenever main-thread code (e.g. debug console commands) needs to mutate `EntityManager` — never call `EntityManager` methods directly from the main thread after `serverLoop.start()`.
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

`engine/render/CameraController` produces a `CameraView` each frame. Three modes, switched via F1/F2/F4 in `main.cpp`:

- **Cockpit mode** (F1): camera at entity position, looking along entity forward + look offset. `setTarget(pos, ori)` + `setCockpitLook(yawDeg, pitchDeg)` each frame. RMB drag accumulates the look offset. Gimbal lock guard: substitutes up=`(1,0,0)` when view direction is near-parallel to world Y.
- **Chase mode** (F2): spherical orbit around entity position. Caller drives `setFreeOrbit(entityPos, yaw, pitch, radius)` each frame with orbit state updated from mouse LMB drag + scroll. Default: 25 m radius, 20° pitch, 180° yaw (behind entity).
- **Free mode** (F4, default): same `setFreeOrbit` orbit math, but pivot is a freely movable world-space reference point (WASD/QE pan, LMB orbit, scroll zoom, R reset). Camera is not attached to any entity.
- **Projection**: infinite reverse-Z perspective hand-built from `f = 1/tan(fovY/2)`: `proj[0][0]=f/aspect`, `proj[1][1]=-f` (Vulkan Y-flip), `proj[2][3]=-1`, `proj[3][2]=near`. `VkRenderer` reads `proj[3][2]` as the near-plane value for shadow cascade split.
- **`CameraView::worldOrigin`** is `glm::dvec3`. `VkRenderer` casts it to `vec3` only at GPU upload sites (shadow cascade center, particle origin).

### World terrain and theaters (feat/world-terrain)

The engine uses a single continuous **world terrain** rather than per-theater heightmap grids. Theaters are geographic regions (bounding boxes in world coordinates) within this single terrain, not separate maps. Players can fly anywhere in the world in one session; theater boundaries are mission conditions, not engine limits.

**Terrain ID:** `"world"` is the canonical ID. fl-base-pack provides global coverage; theater content packs override individual chunks at higher mod priority via `IContentPack::resolveTerrainChunk()`.

**Chunk format:** 16-bit grayscale PNG, 513×513 pixels, **15,360 m per chunk** (512 intervals × 30 m = native Copernicus GLO-30 resolution — no upsampling needed). Three LOD levels: LOD 0 = 513×513, LOD 1 = 257×257, LOD 2 = 129×129. Chunk path convention: `terrain/<id>/lod<n>/chunk_<x>_<y>.png` (all lowercase, 4-digit zero-padded coordinates).

**LOD ring distances** (for a 15,360 m chunk): LOD 0 within 3×3 chunks (~46 km), LOD 1 within 5×5 (~77 km), LOD 2 within 7×7 (~107 km); evict beyond ~120 km. Maximum ~83 chunks in memory at steady state.

**`IContentPack::resolveTerrainChunk(terrainId, chunkX, chunkY, lod) → optional<string>`**: pure virtual — implemented in #172. `FolderContentPack` probes `terrain/<id>/lod<n>/chunk_<x>_<y>.png` under its mod dir (zero-padded 4-digit coords). `AssetManager::resolveTerrainChunk()` walks the priority stack; first-wins, not cached. `TerrainStreamer` calls this to resolve the path before queuing `IAsyncFilesystem::readFileAsync()`. `MockContentPack` in `test_content_system.cpp` has a configurable `chunkPaths` map (key format: `"terrainId:x:y:lod"`) for priority-stack tests; all other test mocks (`EmptyContentPack`, `NullContentPack`, `MockContentPack` in `test_scene_renderer.cpp`) stub it as `return std::nullopt;`.

**`TerrainStreamer`** (`engine/render/TerrainStreamer.h`): manages chunk lifecycle. Constructor: `TerrainStreamer(TerrainManifest, AssetManager&, IAsyncFilesystem&, IRenderer*)` — `IRenderer*` nullable for headless. Inherits `IAsyncFilesystemHandler`; registers/deregisters itself as the sole async FS handler. `update(dvec3 cameraPos)` computes desired chunk set (LOD rings), evicts out-of-range chunks, queues async reads or generates procedural fallback (rate-limited to `kMaxProceduralPerUpdate=8` chunks/frame to avoid first-frame hitch). `getRenderItems(dvec3 worldOrigin) → vector<RenderItem>` returns best-available LOD per (cx,cy) for the 7×7 ring. `heightAt(double x, double z) → double` bilinear interpolation from the LOD0 chunk only; returns 0.0 if not loaded. `surfaceAt()` stub returns 0. `chunkCount() → size_t` exposed for tests. LOD ring semantics: each LOD maintained for its full concentric square — LOD 0: 3×3 = 9 entries, LOD 1: 5×5 = 25 entries, LOD 2: 7×7 = 49 entries; **83 total** (9+25+49), all independent, enabling seamless LOD transitions. Mesh generation uses `buildTerrainMeshGlb()` (see `TerrainMeshBuilder.h`). Single shared `m_terrainMat` for all chunks. Wired in `main.cpp` via `sceneRenderer.setTerrainStreamer(&ts)` and `p.asyncFilesystem->service()` + `ts.update()` each frame. PNG decode (for content-pack chunks) via `decodeTerrainChunkPng()` in `TerrainChunkIO.cpp` — `STB_IMAGE_STATIC` already defined there to prevent ODR conflict with `VkResources.cpp`.

**Terrain rendering:** chunks are submitted as standard `RenderItem`s via `IRenderer::createMesh()`/`createMaterial()` — no new `IRenderer` pure virtuals, no `FrameScene` changes, no `MockRenderer` updates required for Phase 2.

**fl-server** must initialize `FolderContentPack` + `AssetManager` + headless `TerrainStreamer` (null renderer) to serve `heightAt()` for authoritative physics (#174).

**DEM source:** Copernicus GLO-30 (ESA, 30 m global, free). `tools/gen_terrain_chunks.py` (#176) converts GeoTIFF → chunk PNGs at all 3 LOD levels. fl-base-pack `scripts/build_terrain.sh` downloads tiles from AWS Open Data (`s3://copernicus-dem-30m/`) and invokes the tool.

**Wire protocol:** fully documented in `docs/network-protocol.md`. `kProtocolVersion = 1`; `MsgHello` is 4 bytes (first reliable packet on connect; client disconnects on version mismatch); `MsgEntityEntry::pos[3]` is `double` (68 bytes); `MsgConnectAck` is 12 bytes (includes `assignedEntityIdx/Gen`); `MsgClientInput` is 44 bytes (client→server, reliable); `MsgLanBeacon` is 74 bytes (`MsgId::LanBeacon=0x10`, raw UDP broadcast, not ENet — received by `DiscoveryListener`). Always parse with `std::memcpy`. #92 (version negotiation), #142 (authoritative game protocol), #170 (double-precision positions), and #91 (LAN discovery) are all merged.

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
- Python scripts (`.py`) are NOT covered by REUSE.toml — every new `.py` file needs inline `# SPDX-FileCopyrightText:` + `# SPDX-License-Identifier:` headers or REUSE CI fails.
<!-- REUSE-IgnoreEnd -->
- Python tool conventions: guard GDAL/heavy imports with `try/except ImportError` so `--help` and pytest unit tests work without the system package installed; unit-test pure-Python logic (no I/O, no GDAL) in `tests/test_<tool>.py` using pytest; add the system package + `python3-pytest` to the Ubuntu CI install step and add both a `pytest` unit-test step and a GDAL synthetic-fixture smoke test step (Ubuntu only).
- All code must compile on Windows (MSVC 2022), Linux (GCC/Clang), macOS (Apple Clang)
- `CMAKE_COMPILE_WARNING_AS_ERROR=ON` in debug builds — fix all warnings

## Key Files

- `README.md` — project overview and documentation index
- `docs/architecture.md` — engine architecture overview
- `docs/development.md` — build prerequisites per platform
- `docs/sandbox.md` — developer key map (camera/flight controls) + debug console command reference
- `GOVERNANCE.md` — decision-making and RFC process
- `CMakePresets.json` — all build presets (debug / release / coverage / asan / msvc variants)
- `platform/RenderTypes.h` — GPU-agnostic POD types: `MeshHandle`, `TextureHandle`, `MaterialHandle`, `CameraView`, `RenderItem`, `FrameScene`, `EnvironmentState`, `ParticleEmitterState`, `FrameStats`, `HudElement` (2D HUD draw command: Text/Line/Rect at normalized 0–1 screen positions with RGBA color); also `RendererVsyncMode` + `RendererSettings`
- `platform/IRenderer.h` — `getFrameStats() const` + `setOverlayLines(span<string_view>)` (top-left monospace debug text) + `submitHudElements(span<HudElement>)` (2D HUD over swapchain); any mock must implement all three. **HUD font is IBM CP437 8×16** (`platform/vulkan/OverlayFont.h`); `HudElement::text` must be **ASCII-only** (bytes 0x20–0x7E) — non-ASCII UTF-8 bytes are treated as raw CP437 glyph indices and render as garbled characters. Unifont upgrade (#203) is the planned fix. `submitHudElements` replaces (not appends) — merge all HudElement spans before the single call.
- `engine/flight/BuiltinFlightModel.h` — compiled-in UFO-like `FlightModelData` singleton for the zero-content-pack sandbox; 800 kg, `S=30 m²`, no stall (`alpha_stall=90°`), T/W≈4, near-infinite fuel; returned via `BuiltinFlightModel::get()`
- `engine/perf/PerformanceOverlay.h` — F3 overlay (Off/Compact/Full); `update(FrameStats, entityCount, simTickMs)` + `lines()` → span for `IRenderer::setOverlayLines`; `OverlayMode` lives in `engine/config/DebugSettings.h`
- `engine/debug/DebugCommandRegistry.h` — whitespace tokenizer + command dispatch; `registerCommand(name, help, handler)` / `dispatch(line)` / `helpText()` / `helpFor(name)`
- `engine/debug/DebugConsole.h` — Quake-style drop-down HUD overlay; backtick toggle (SDL_SCANCODE_GRAVE in main.cpp); `ITextInputHandler` impl; output ring (64 entries); position widget (`toggle_pos`); `buildHud(playerPos*)` / `elements()` / `showPosRef()` / `openHeadless()` (test helper); no SDL dep (uses IInput for special keys)
- `engine/debug/DebugCommands.h` — `DebugCommandContext` struct + `registerBuiltinCommands(registry, ctx)`; registers help/types/entities/spawn/kill/tp/toggle_pos and stubs for set_weather/set_difficulty/reload_content; spawn/kill/tp enqueue via `GameLoop::enqueueSimCallback`; entities reads `SimRenderBridge::current()` (main-thread-safe); `docs/sandbox.md` has the full command reference
- `engine/config/DebugSettings.h` — `OverlayMode` enum + `DebugSettings` struct; persisted as `[debug].overlay_mode` in user.toml
- `engine/render/RenderSnapshot.h` — `EntityRenderEntry` + `RenderSnapshot`; POD only, no engine-entity headers (uses raw uint32_t/uint8_t to avoid circular deps)
- `engine/render/SimRenderBridge.h` — lock-free triple-buffer bridge; `publish()` sim-thread-only, `tryAdvance()`/`current()`/`hasSnapshot()` render-thread-only; `publishExternal()` main-thread-only for network-client mode
- `engine/render/SceneRenderer.h` — snapshot→FrameScene bridge; `renderFrame(alpha, camera, env, extraEmitters={})` between beginFrame/endFrame; handles mesh upload/cache, camera-relative transforms, damage variant, front-to-back sort, draw-distance cull; `setParticleSystem(ps, effectResolver)` wires damage-effect emission; `setDrawDistance(km)` sets entity cull distance; `setBuiltinFloor(bool)` enables the 4 km floor plane; `setTerrainStreamer(TerrainStreamer*)` appends terrain chunk RenderItems each frame (terrain replaces builtin floor in sandbox)
- `engine/render/BuiltinGeometry.h` — `builtinTetrahedronGlb()` + `builtinFloorPlaneGlb()`: embedded geometry for the no-content-pack sandbox; generated by `tools/gen_builtin_glb.py`. `builtinWorldTerrainManifest()`: hardcoded `TerrainManifest` for the infinite procedural world (originX/Z = -7680, gridWidth/Height = -1).
- `engine/render/TerrainManifest.h` — POD struct `TerrainManifest` (terrainId, chunkSizeM, gridWidth/Height, originX/Z); shared by `BuiltinGeometry.h` and `TerrainStreamer.h`.
- `engine/render/TerrainChunkIO.h` — `decodeTerrainChunkPng()`: decode 16-bit grayscale PNG from memory (for content-pack chunks); `writeTerrainChunkCache()` / `readTerrainChunkCache()`: binary `.u16` cache format (magic FLCH + uint16 width/height + row-major uint16 data) for engine-generated procedural chunks.
- `engine/render/ProceduralTerrainChunk.h` — `generateProceduralChunk(cx, cy, manifest, params)`: world-space FBM heightmap generator; returns 513×513 uint16 array; `kBuiltinProceduralParams` (base 550 m, amplitude 150 m, freq 30 km); seamless across chunk boundaries. User cache path uses 6-digit index + bias 100000 (e.g. `chunk_100000_100000.u16`).
- `engine/render/TerrainMeshBuilder.h` — `buildTerrainMeshGlb(heights, hmSize, meshGrid, chunkSizeM) → vector<uint8_t>`: runtime binary glTF 2.0 generator from a uint16 heightmap; POSITION + NORMAL (VEC3 float32), UNSIGNED_SHORT indices, non-interleaved bufferViews; vertices in chunk-local space (Y = elevation − 32768 m). Mesh grids per LOD: 128×128, 64×64, 32×32 quads. Returns empty on invalid input.
- `engine/render/TerrainStreamer.h` — async chunk lifecycle manager; `update(dvec3)`, `getRenderItems(dvec3)`, `heightAt(double,double)→double`, `surfaceAt(double,double)→uint8_t`, `chunkCount()→size_t`; implements `IAsyncFilesystemHandler`; #173
- `tools/terrain-chunk-io` — C++ CLI tool linking `engine-render`; `decode --input file.png --output file.u16` and `gen-procedural --cx N --cy N --output file.u16`; smoke-tested in CI via `--version`.
- `engine/net/GameProtocol.h` — seven packed wire types; `kProtocolVersion = 1`; `MsgHello` 4 bytes (`MsgId::Hello = 0x00`); `MsgConnectAck` 12 bytes (+assignedEntityIdx/Gen), `MsgEntityEntry` 68 bytes (double pos; offset 66 = `throttle` uint8_t [0–100]; offset 67 = `fuelPct` uint8_t [0–100]), `MsgClientInput` 44 bytes (`MsgId::ClientInput = 0x03`); `MsgWorldSnapshotHeader` offset 1 = `protocolVersion uint8_t`; `MsgClientInput` offsets 2–3 = `protocolVersion uint16_t`; `MsgLanBeacon` 74 bytes (`MsgId::LanBeacon = 0x10`, raw UDP only); `kGameModeCampaign/Mission/Sandbox` bitmask constants; always parse with `memcpy`; all documented field offsets protected by `offsetof` static_asserts (requires `<cstddef>`); see `docs/network-protocol.md`
- `engine/net/WorldBroadcaster.h` — `ISimUpdate` + `INetworkEventHandler`; spawns/kills peer entities and their `FlightIntegrator` instances on connect/disconnect; decodes `MsgClientInput` → `PeerInputState`; steps each peer's `FlightIntegrator` each tick then calls `EntityManager::onTick`; `m_peerEntities` + `m_peerInputs` + `m_peerFlightSims` maps (keyed by peerId); `getPeerCount() const noexcept` reads `std::atomic<int> m_activePeerCount` — safe cross-thread read for the LAN beacon
- `engine/net/DiscoveryBeacon.h` — LAN discovery sender; dual-socket (IPv4 broadcast + IPv6 `ff02::1` multicast); `Config` struct with name/port/maxPlayers/gameModeFlags/intervalMs/broadcastAddr; `tick(playerCount)` fires on first call then every `intervalMs`; `isOpen()` true if at least one socket succeeded; part of `engine-net`
- `engine/net/DiscoveryListener.h` — LAN discovery receiver; `ServerInfo` embeds `fl::MsgLanBeacon beacon` + `address` + `lastSeen`; deduplicates by `(gamePort, name)`, prefers IPv4 over IPv6 link-local address; TTL pruning on `poll()`; `servers()` returns snapshot; part of `engine-net`
- `docs/network-protocol.md` — stable public wire protocol spec: channel assignments, struct field tables (Offset/Size/Field/Type/Notes), little-endian declaration, ARM64 alignment warnings for misaligned fields (`pos[3]` at offset 12, `tickIndex` at offset 4 in snapshot header), version negotiation implemented (#92) via `MsgHello` handshake + per-packet `protocolVersion` echo, `MsgLanBeacon` raw UDP beacon (#91), bandwidth/MTU scalability guidance (single-fragment threshold ≈20 entities; interest management required for 128+ players)
- `engine/render/CameraController.h` — three camera modes (`Cockpit` F1, `Chase` F2, `Free` F4); `setFreeOrbit(pivot,yaw,pitch,dist)` drives Chase+Free; `setTarget(pos,ori)` + `setCockpitLook(yawDeg,pitchDeg)` drives Cockpit; `view(aspectRatio)` → `CameraView`; infinite reverse-Z projection with Vulkan Y-flip
- `engine/render/FlightHud.h` — minimal in-flight HUD for the zero-content-pack sandbox; `update(EntityRenderEntry*)` (pass nullptr when not in Cockpit mode) → `elements()` → `span<HudElement>` for `IRenderer::submitHudElements`; shows IAS/ALT left, HDG bottom, THR/FUEL right in bright military green; red damage warning at center
- `engine/render/ParticleSystem.h` — `ParticlePreset` + `ParticleSystem`; preset registry, per-frame emit/reset/emitters() accumulator; `DamagePenalty::visualEffect` maps to preset name
- `cmake/dependencies.cmake` — all FetchContent declarations; GLM is unconditional, Vulkan-specific deps are gated on `Vulkan_FOUND`
- `platform/vulkan/VkRendererFactory.h` — thin factory header; only include needed by game/tools to instantiate the renderer
- `tools/blender_gen.py` — headless Blender 4.x parametric aircraft mesh generator; `blender --background --python tools/blender_gen.py -- --id <id> --output-dir <dir> [--wing-style delta|swept|straight] [--lod] [--bake-textures]`; outputs `<id>.glb` (clean + `_b` node), `<id>_dmg.glb`, optional LODs + PNGs; `.ktx2` URIs pre-wired in GLB JSON for tex-compress
- `tools/gen_terrain_chunks.py` — GDAL-based terrain chunk pipeline (#176); converts any GeoTIFF/VRT to 16-bit grayscale PNG chunks at `terrain/<id>/lod<n>/chunk_<xxxx>_<yyyy>.png`; auto-detects UTM CRS from centroid (or `--srs EPSG:NNNNN`); 3 LOD levels via strided subsampling; parallel via `ProcessPoolExecutor` (`--workers N`); `--write-manifest` outputs `terrain/<id>.json`; `--skip-existing` for resumable planet-scale runs. Install: `apt install python3-gdal`. Tests: `tests/test_gen_terrain_chunks.py`.
