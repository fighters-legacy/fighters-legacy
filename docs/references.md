# Technology Reference Index

This page maps every technology used in fighters-legacy to its canonical upstream
documentation. It is the recommended starting point for contributors who are new to one
or more of the libraries or tools in the stack.

For competitive landscape context — what simulators exist and where fighters-legacy fits
relative to them — see [`docs/prior-art.md`](prior-art.md).

---

## Engine Technologies

Runtime libraries the engine is built on.

| Technology | Role in this project | Documentation |
|---|---|---|
| Vulkan 1.3 | GPU rendering API (`platform/vulkan/`) | [Khronos spec](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html) · [vulkan-tutorial.com](https://vulkan-tutorial.com) |
| MoltenVK | Vulkan-over-Metal ICD for macOS | [KhronosGroup/MoltenVK](https://github.com/KhronosGroup/MoltenVK) · [LunarG SDK](https://vulkan.lunarg.com/) |
| GLM | Vector/matrix/quaternion math shared across engine, renderer, and tests | [GLM manual](https://glm.g-truc.net/0.9.9/api/index.html) · [g-truc/glm](https://github.com/g-truc/glm) |
| VulkanMemoryAllocator | GPU memory allocation for all Vulkan buffers and images (`platform/vulkan/`) | [GPUOpen VMA docs](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/) |
| KTX-Software / Basis Universal | KTX2 texture loading and Basis→BC7/ASTC transcoding at runtime (`platform/vulkan/`) | [KhronosGroup/KTX-Software](https://github.com/KhronosGroup/KTX-Software) · [Basis Universal](https://github.com/BinomialLLC/basis_universal) |
| tinygltf | glTF 2.0 mesh parsing in the Vulkan renderer and validate-mesh tool | [syoyo/tinygltf](https://github.com/syoyo/tinygltf) · [glTF spec](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html) |
| SDL3 | Windowing, input, Vulkan surface creation (`platform/sdl3/`) | [SDL3 wiki](https://wiki.libsdl.org/SDL3/FrontPage) |
| OpenAL Soft | 3D positional audio (`platform/openal/`) | [openal-soft.org](https://openal-soft.org) |
| ENet | Reliable UDP networking (`platform/net/`) | [enet.bespin.org](http://enet.bespin.org) |

---

## Rendering Techniques

Algorithms the Vulkan renderer implements (see [`docs/rendering.md`](rendering.md) for the pass-by-pass
graph). "Role" notes whether a technique is in the renderer today or planned under a tracking issue.

| Technique | Role in this project | Reference |
|---|---|---|
| Cook-Torrance PBR (GGX NDF, Smith geometry, Schlick Fresnel) | Opaque material shading (`mesh.frag`) | [Karis, *Real Shading in UE4* (SIGGRAPH 2013)](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf) |
| PSSM cascaded shadow maps | Sun shadows (`shadow.vert`, `mesh.frag`) | [Zhang et al., *Parallel-Split Shadow Maps* (GPU Gems 3)](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus) |
| Octahedral normal encoding | World-space normal G-buffer packing (`mesh.frag`) | [Cigolle et al., *A Survey of Efficient Representations for Unit Vectors* (JCGT 2014)](https://jcgt.org/published/0003/02/01/) |
| GTAO / horizon-based ambient occlusion | Ambient occlusion (`gtao.comp`) | [Jiménez et al., *Practical Realtime Strategies for Accurate Indirect Occlusion* (SIGGRAPH 2016)](https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf) · [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO) (planned: half-res + temporal, #448) |
| Analytic atmospheric scattering + aerial perspective | Atmospheric sky + distance scattering (`sky.frag`, `mesh.frag`) | [Hillaire, *A Scalable and Production Ready Sky and Atmosphere* (EGSR 2020)](https://sebh.github.io/publications/egsr2020.pdf) · [Bruneton & Neyret, *Precomputed Atmospheric Scattering*](https://hal.inria.fr/inria-00288758/document) (planned: precomputed LUTs, #445) |
| Khronos PBR Neutral tonemapper | HDR → LDR tonemap (`tonemap.frag`) | [KhronosGroup/ToneMapping](https://github.com/KhronosGroup/ToneMapping) |
| FXAA | Post-process anti-aliasing (`tonemap.frag`) | [Lottes, *FXAA* (NVIDIA whitepaper)](https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf) |
| Bloom (bright-pass + separable Gaussian) | HDR glow (`bloom_*.frag`) | [Jimenez, *Next Generation Post Processing in Call of Duty: Advanced Warfare* (SIGGRAPH 2014)](https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare) |
| TAA + CAS | Temporal AA + sharpening (planned, #443) | [Karis, *High-Quality Temporal Supersampling* (SIGGRAPH 2014)](https://advances.realtimerendering.com/s2014/) · [AMD FidelityFX CAS](https://gpuopen.com/fidelityfx-cas/) |
| Temporal upscaling | Render-scale upscaling (planned, #450) | [AMD FSR 2](https://gpuopen.com/fidelityfx-superresolution-2/) · [Intel XeSS](https://github.com/intel/xess) |
| Image-based lighting (split-sum) | Sky-driven ambient (planned, #452) | [Karis, *Real Shading in UE4* — split-sum approximation](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf) |

---

## Build & Tooling

| Tool | Role in this project | Documentation |
|---|---|---|
| CMake 3.25+ / presets | Build system; all platforms use `CMakePresets.json` | [CMake presets reference](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) |
| glslang / `glslangValidator` | GLSL-to-SPIR-V compiler; invoked at configure time to build Vulkan shaders | [KhronosGroup/glslang](https://github.com/KhronosGroup/glslang) |
| Catch2 v3 | Unit test framework (`tests/`) | [Catch2 tutorial](https://github.com/catchorg/Catch2/blob/devel/docs/tutorial.md) |
| toml++ | TOML config parsing (`server.toml`, `user.toml`, mod manifests) | [marzer.github.io/tomlplusplus](https://marzer.github.io/tomlplusplus/) |
| yaml-cpp | YAML parsing in the `validate-mission` asset pipeline tool | [jbeder/yaml-cpp](https://github.com/jbeder/yaml-cpp) |
| clang-format | Code formatting; CI enforces on every PR | [ClangFormat docs](https://clang.llvm.org/docs/ClangFormat.html) |
| REUSE / SPDX | License compliance tooling; CI enforces via `fsfe/reuse-action` | [reuse.software](https://reuse.software) |
| git-cliff | Changelog generation for releases | [git-cliff.org](https://git-cliff.org) |

---

## Flight Sim Domain

This section is for contributors unfamiliar with combat flight simulation concepts.
For landscape context and motivation, see [`docs/prior-art.md`](prior-art.md).

### Flight model concepts

fighters-legacy uses a simplified 6-DOF stability-derivative flight model. The design
rationale is in `docs/design.md` and the FDM RFC issue. The best publicly available
reference for this mathematical approach is the JSBSim documentation:

- [JSBSim reference manual](https://jsbsim-team.github.io/jsbsim-reference-manual/) —
  the most rigorous open-source treatment of stability-derivative 6-DOF models.
  fighters-legacy does not use JSBSim itself, but borrows its coefficient structure and
  AoA/Mach-indexed table approach.

### Jane's Fighters Anthology

The direct spiritual ancestor of this project. The original Jane's FA manual covers the
aircraft roster, weapons systems, and mission concepts that define the fidelity target
for content packs. The manual is included with the GOG.com release of Jane's Combat
Simulations.

### Community documentation

The DCS World community has produced the most comprehensive publicly available
documentation on modern combat aircraft systems modelling:

- [Hoggit community wiki](https://wiki.hoggitworld.com/) — reference cards, systems
  guides, and flight model notes for DCS aircraft; useful background for contributors
  working on avionics or flight model design.
- [Eagle Dynamics forums](https://forums.eagle.ru/) — primary discussion venue for DCS
  flight model questions; archived threads cover the modelling methodology in depth.
