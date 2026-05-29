# Fighters Legacy — Claude Code Instructions

## Project Overview

GPL v3 general-purpose combat flight sim engine, inspired by Jane's Fighters Anthology (1998).
Cross-platform: Windows 10/11, Linux, macOS. Phase 2 (Modern-Particles Engine) is active.

## Architecture

```
engine/         — core: content system, asset manager, IContentPack interface
engine/entity/  — entity/object system: pool, type registry, damage model, EntityManager
engine/render/  — sim→render bridge: RenderSnapshot, SimRenderBridge (lock-free triple-buffer)
platform/       — HAL: Vulkan, SDL3, OpenAL Soft, ENet backends
platform/RenderTypes.h — GPU-agnostic scene types shared across the HAL boundary
game/           — fighters-legacy game binary
tools/          — developer utilities; asset pipeline (validate-flight-model, validate-mission, validate-licenses, validate-mesh, tex-compress)
tests/          — Catch2 unit tests
```

The engine is fully content-agnostic. It knows nothing about FA or any specific game.
FA support lives in jomkz/fa-content. No FA-specific code belongs in this repo.

### Math library

**GLM** is the shared vector/matrix/quaternion library, linked as an INTERFACE dependency on `platform-hal`. Anything that links `platform-hal` (engine, game, tests) gets GLM automatically. Use `glm::vec3`, `glm::mat4`, `glm::quat`, etc.

### Renderer architecture (Phase 2)

`VkRenderer` (platform/vulkan/) uses Vulkan 1.3 dynamic rendering (`VK_KHR_dynamic_rendering`) — no `VkRenderPass` or `VkFramebuffer` objects. Four passes per frame:

1. **Shadow** — `kNumCascades=4` PSSM cascades rendered into a `kShadowRes=2048` 2D depth array (`VK_FORMAT_D32_SFLOAT`, **forward-Z**, depth clear = 1.0). Cascade matrices computed via tight bounding-sphere fit; PCF comparison sampler (`VK_COMPARE_OP_LESS_OR_EQUAL`). `ShadowUBO` bound at set 0, binding 2; `sampler2DArrayShadow` at set 0, binding 3.
2. **Forward** — Cook-Torrance PBR (GGX NDF, Smith geometry, Schlick Fresnel) with normal maps + ORM textures (set 1: base color / normal / ORM at bindings 0–2). Geometry into HDR offscreen (`VK_FORMAT_R16G16B16A16_SFLOAT`) with **reverse-Z** depth (`VK_FORMAT_D32_SFLOAT`, far = 0.0, depth clear = 0.0, compare = GREATER).
3. **Sky** — gradient + sun disc via fullscreen triangle (`tonemap.vert`) with `GREATER_OR_EQUAL` depth test, depth write off; renders only where depth == 0.0 (reverse-Z far, no geometry drawn). `SkyPushConstants` (96 bytes, fragment only): `invViewProj + sunDirection + sunColor`.
4. **Tonemap** — Khronos PBR Neutral, fullscreen HDR → swapchain (`B8G8R8A8_SRGB`).

**Note:** shadow passes use forward-Z (near=0, far=1); scene depth uses reverse-Z. These are independent depth spaces.

World convention: right-handed, Y-up, meters (matches glTF). Vulkan clip-space Y-flip handled in the projection matrix. Camera-relative rendering rebases transforms to the camera origin before GPU upload (float32-safe at arbitrary theater scale).

**Texture upload:** KTX2 Basis Universal → BC7 (desktop, if `VK_FORMAT_BC7_UNORM_BLOCK` supported) → ASTC 4×4 (Apple Silicon, if BC7 absent) → RGBA32 fallback. All mip levels uploaded via `createGpuImageCompressed` using `ktxTexture_GetImageOffset` per mip. sRGB/UNORM views chosen per texture semantic (base color = sRGB, normal/ORM = UNORM). Normal maps use tangent-space flat normal default `{128,128,255}`; ORM defaults to all-ones linear white.

Runtime shader discovery: `VkRenderer::resolveShaderDir()` tries `SDL_GetBasePath()` + `"shaders/"` first, then macOS `.app` bundle path, then the build-tree `FL_SHADER_DIR` fallback. Release packages must stage `*.spv` into `dist/shaders/` (see `release.yml`).

**Renderer instantiation:** game and tool code must use `createVulkanRenderer()` from `platform/vulkan/VkRendererFactory.h` — never include `VkRenderer.h` directly. `VkRenderer.h` pulls in `VkResources.h` → `vk_mem_alloc.h`, which is only on the private include path of `platform-vulkan`.

**GLM extension headers:** `VkRenderer.cpp` requires `<glm/gtc/matrix_transform.hpp>` (for `glm::lookAt`) and `<glm/ext/matrix_clip_space.hpp>` (for `glm::orthoZO`). `engine/render/RenderSnapshot.h` and `engine/entity/EntityManager.cpp` require `<glm/gtc/quaternion.hpp>` (for `glm::quat`). These are not in `<glm/glm.hpp>` core — always include them explicitly.

### Sim→render bridge (Phase 2, PR 4)

`engine/render/SimRenderBridge` is a **lock-free triple-buffer** that ships a per-tick entity snapshot from the sim thread to the render thread. Three `RenderSnapshot` slots rotate: one owned by the sim, one in the atomic spare, one held by the render thread. `publish()` moves the completed snapshot into the spare (release fence); `tryAdvance()` atomically swaps the render slot with the spare when a newer tick is available (acq_rel fence). All three slot indices are always a distinct permutation of {0,1,2}.

- `EntityRenderEntry`: entityIdx, entityGen, typeIndex, position (glm::vec3), orientation (glm::quat, w-first constructor), velocity (glm::vec3 for sub-tick extrapolation), damageLevel (uint8_t), playerOwned.
- `EntityManager::setRenderBridge(SimRenderBridge*)` — call before `GameLoop::start()`; `onTick` publishes after `reapDeadEntities()` so dead slots are excluded.
- `engine-render` CMake library is **unconditional** (no Vulkan dep) — builds in CI without a GPU. `engine-entity` privately links `engine-render`; any binary/test that links `engine-entity` gets `engine-render` resolved automatically.

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
<!-- REUSE-IgnoreEnd -->
- All code must compile on Windows (MSVC 2022), Linux (GCC/Clang), macOS (Apple Clang)
- `CMAKE_COMPILE_WARNING_AS_ERROR=ON` in debug builds — fix all warnings

## Key Files

- `README.md` — project overview and documentation index
- `docs/architecture.md` — engine architecture overview
- `docs/development.md` — build prerequisites per platform
- `GOVERNANCE.md` — decision-making and RFC process
- `CMakePresets.json` — all build presets (debug / release / coverage / asan / msvc variants)
- `platform/RenderTypes.h` — GPU-agnostic POD types: `MeshHandle`, `TextureHandle`, `MaterialHandle`, `CameraView`, `RenderItem`, `FrameScene`, `EnvironmentState`, `ParticleEmitterState`
- `engine/render/RenderSnapshot.h` — `EntityRenderEntry` + `RenderSnapshot`; POD only, no engine-entity headers (uses raw uint32_t/uint8_t to avoid circular deps)
- `engine/render/SimRenderBridge.h` — lock-free triple-buffer bridge; `publish()` sim-thread-only, `tryAdvance()`/`current()`/`hasSnapshot()` render-thread-only
- `cmake/dependencies.cmake` — all FetchContent declarations; GLM is unconditional, Vulkan-specific deps are gated on `Vulkan_FOUND`
- `platform/vulkan/VkRendererFactory.h` — thin factory header; only include needed by game/tools to instantiate the renderer
