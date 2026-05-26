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
| SDL3 | Windowing, input, Vulkan surface creation (`platform/sdl3/`) | [SDL3 wiki](https://wiki.libsdl.org/SDL3/FrontPage) |
| OpenAL Soft | 3D positional audio (`platform/openal/`) | [openal-soft.org](https://openal-soft.org) |
| ENet | Reliable UDP networking (`platform/net/`) | [enet.bespin.org](http://enet.bespin.org) |

---

## Build & Tooling

| Tool | Role in this project | Documentation |
|---|---|---|
| CMake 3.25+ / presets | Build system; all platforms use `CMakePresets.json` | [CMake presets reference](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) |
| glslang / `glslangValidator` | GLSL-to-SPIR-V compiler; invoked at configure time to build Vulkan shaders | [KhronosGroup/glslang](https://github.com/KhronosGroup/glslang) |
| Catch2 v3 | Unit test framework (`tests/`) | [Catch2 tutorial](https://github.com/catchorg/Catch2/blob/devel/docs/tutorial.md) |
| toml++ | TOML config parsing (`server.toml`, `user.toml`, mod manifests) | [marzer.github.io/tomlplusplus](https://marzer.github.io/tomlplusplus/) |
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
