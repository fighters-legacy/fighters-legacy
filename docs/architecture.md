# Architecture Overview

fighters-legacy is a general-purpose combat flight sim engine. The engine is fully **content-agnostic** — it has no knowledge of any specific game, franchise, or asset format. All game content enters the engine through a single boundary: the `IContentPack` interface.

## Layered model

```
┌───────────────────────────────────────┐
│           Content Pack(s)             │  ← IContentPack implementors (external repos)
├───────────────────────────────────────┤
│           Content System              │  ← engine/content/ — AssetManager, ModLoader
├───────────────────────────────────────┤
│           Engine Core                 │  ← engine/ — game loop, entity system, flight model,
│                                       │    AI runtime, mission loader
├───────────────────────────────────────┤
│           Platform HAL                │  ← platform/ — Vulkan, SDL3, OpenAL Soft, ENet
└───────────────────────────────────────┘
              Host OS / Hardware
```

### Platform HAL (`platform/`)

Thin abstraction over OS and hardware APIs. Each backend is isolated:

| Module | Backend | Path |
|---|---|---|
| Renderer | Vulkan 1.3 + MoltenVK | `platform/vulkan/` |
| Windowing / Input | SDL3 | `platform/sdl3/` |
| Audio | OpenAL Soft | `platform/openal/` |
| Networking | ENet | `platform/net/` |

The HAL exposes platform-independent interfaces to the engine core. Nothing above the HAL layer links directly against Vulkan, SDL3, OpenAL, or ENet headers.

#### Platform HAL Interfaces

All interfaces live under `platform/` and are exposed via the `platform-hal` CMake INTERFACE library. Engine core and backends link against `platform-hal` rather than including headers by path.

| Interface | Header | Purpose |
|---|---|---|
| `IWindowEventHandler` | `platform/IWindowEventHandler.h` | Callback target for window events (resize, close); implemented by the engine game loop |
| `IWindow` | `platform/IWindow.h` | Create/destroy OS window, pump events, query dimensions, expose native handle for surface creation |
| `IRenderer` | `platform/IRenderer.h` | Render frame lifecycle: init, beginFrame, endFrame, shutdown |
| `IAudio` | `platform/IAudio.h` | Buffer upload, source play/stop/position/velocity, listener transform/velocity, source-relative (non-positional) mode |
| `ITextInputHandler` | `platform/IInput.h` | Callback target for OS text input and IME composition events; implemented by any UI component that accepts free-form text |
| `IInput` | `platform/IInput.h` | Keyboard, mouse, and gamepad state (SDL3 GameController API); drives text input mode via `ITextInputHandler` |
| `INetworkEventHandler` | `platform/INetwork.h` | Callback target for network events (connect, disconnect, receive); implemented by the multiplayer subsystem |
| `INetwork` | `platform/INetwork.h` | UDP transport: bind/connect, send/recv, peer state, frame pump |
| `IFilesystem` | `platform/IFilesystem.h` | Synchronous file I/O and directory scan over two path domains (Assets, UserData) |
| `ILogger` | `platform/ILogger.h` | Structured logging routed to the platform-native output |

**Event handler pattern:** `IWindowEventHandler`, `INetworkEventHandler`, and `ITextInputHandler` are separate interfaces registered with their respective `IWindow` / `INetwork` / `IInput` instances. The engine implements the handler; the platform backend calls it during `pollEvents()` / `service()` / text input events. This keeps platform-to-engine callbacks decoupled without requiring the backend to know anything about the game loop.

**Wiring — `Platform` struct:** `platform/Platform.h` defines a plain aggregate struct holding `std::unique_ptr` to each interface. The platform entry point (e.g. `platform/sdl3/`) constructs a `Platform`, populates it with concrete backend instances, and passes it to the engine on startup. The engine holds `Platform` by value and owns all interface lifetimes. Backends can be mixed freely — a test build might use a null renderer stub alongside a real filesystem backend.

**Design rules for all HAL interfaces:**
- No platform-specific headers (`SDL3`, `vulkan.h`, `al.h`, `enet.h`) may appear in any interface file.
- `IWindow::nativeHandle()` returns `void*` — the only platform type that crosses the boundary, and it does so as an opaque pointer used only by the Vulkan backend internally.
- All interface methods are pure virtual. No implementation code lives in these headers.
- Interfaces that can fail during init expose `getLastError() const → const char*` for human-readable diagnostics.
- **Thread safety:** `ILogger::log` is the only HAL method guaranteed thread-safe. All other methods on all other interfaces must be called from the main thread.

**`IRenderer` scope (Phase 1):** `IRenderer` is lifecycle-only — init, frame begin/end, shutdown. Scene submission (mesh handles, transforms, materials) and a render graph abstraction are added in the Vulkan backend workstream (Phase 2).

**`IFilesystem` is synchronous:** `readFile` blocks until the OS delivers the data. It is correct for startup asset loading, mod discovery, and config reads. It must not be called on the main thread for per-frame terrain streaming. Async file I/O is a separate Phase 2 design item.

### Engine Core (`engine/`)

Game logic and simulation, independent of any specific content:

- **Game loop** — fixed-timestep update, variable-rate render
- **Entity system** — component-based scene graph
- **Flight model** — aerodynamics simulation
- **AI runtime** — Lua-scripted AI behaviours
- **Mission loader** — scenario and campaign structure
- **Localization** — keyed string lookup with BCP 47 fallback chain, `{placeholder}` interpolation, plural forms, and RTL metadata; see `engine/i18n/`

### Content System (`engine/content/`)

Bridges the engine core to external content:

- **`IContentPack`** — the single interface all content packs must implement. Exposes asset loading, mission data, and configuration. The engine calls only this interface; it never knows what implements it.
- **`AssetManager`** — caches and serves assets requested by the engine core via active content packs.
- **`ModLoader`** — discovers and loads `IContentPack` implementors at runtime (shared libraries).

Mods can ship translations by placing TOML files under `locale/<lang>/` inside their mod directory. The `Localization` system merges these with the engine's base `locale/` files; higher-priority mods win on key conflicts. See [docs/modding/localization.md](modding/localization.md).

### Content Packs (external)

Any repository that implements `IContentPack` and compiles to a shared library can be loaded as a content pack. The engine is indifferent to the source or format of the underlying assets.

## Locked Architectural Decisions

These decisions are finalized and not subject to revision without an RFC.

| Concern | Choice | Rationale |
|---|---|---|
| Rendering | Vulkan + MoltenVK | One API everywhere; MoltenVK → Metal on Apple Silicon |
| Windowing / input | SDL3 | Wayland + modern controller support; long-term path |
| Audio | OpenAL Soft | Positional 3D audio; native music in OGG; no MIDI dependency in engine core |
| Network transport | ENet 1.3.x (reliable UDP) | Reliable + unreliable channels; congestion control; cross-platform. **IPv4 only** — ENet 1.3.x does not support IPv6; dual-stack requires the `enet6` fork (SirLynix/enet6), which is not a Phase 1 concern. |
| Build system | CMake 3.25+ | Cross-platform from day one |
| Engine repo | `fighters-legacy` (this repo) | Separate from fighters-toolkit |
| Content system | Plugin / content-pack architecture | Each content source = one plugin; mods = other plugins; engine core has zero content dependency |
| Native 3D models | glTF 2.0 | Royalty-free; Blender export; industry standard |
| Native textures | PNG (source) + KTX2/DDS (GPU) | Mipmaps, BC compression; toolchain converts PNG → KTX2 at pack time |
| Native audio | OGG Vorbis / Opus | Open, compressed, widely supported |
| Native flight model | TOML | Human-readable, structured, easily diffable |
| Native missions | YAML | Human-readable, tool-friendly |
| Native campaigns | YAML | Arbitrary theater graph; no FA 6-theater limit |
| Native terrain | Streaming heightmap chunks + JSON | No tile-count cap; supports large theaters |
| Native AI scripts | Lua 5.4 | Embeddable, sandbox-able, moddable |
| Multiplayer topology | `fl-server` dedicated binary + `fl-lobby` REST service | Server-authoritative; no P2P player-count cap; self-hostable |
| Entity system | Dynamic pool, no hard caps | No fixed object count limit |
| License | GPL v3 | Engine modifications must stay open source; protects community investment |
| Hosting | GitHub, public repository | Unlimited Actions CI on public repos; GitHub Free sufficient |

## Content Pack Architecture

This is the central design decision that affects every other phase. **The engine core has no dependency on any content library.** All asset access goes through an `IContentPack` interface.

### Mods vs Plugins

Content for Fighters Legacy comes in two forms.

**Mods (Lua + assets)** are directories dropped into `mods/<name>/` containing a `manifest.toml` plus any combination of Lua AI scripts, glTF meshes, TOML flight model and weapon data, YAML missions and campaigns, OGG audio, and PNG / KTX2 textures. **Most user content — reskins, missions, new aircraft, custom campaigns — is a mod.** No C++ compiler required.

**Plugins (compiled content packs)** are shared libraries (`.dll` / `.so` / `.dylib`) that implement the `IContentPack` interface. GPL v3 applies to compiled plugins unless a linking exception is granted. **Install compiled plugins only from authors whose source you can verify.**

### IContentPack Interface

```
engine/content/IContentPack.h
    name(), version(), priority()
    init()              → Status { Ready | NeedsConfiguration }
    configure(IWindow*) → bool
    hasAsset(name, AssetType) → bool
    loadMesh(name)        → MeshData
    loadTexture(name)     → TextureData
    loadAudio(name)       → AudioBuffer
    loadFlightModel(name) → FlightModel
    loadMission(name)     → MissionData
    loadTerrain(name)     → TerrainData
    loadAIScript(name)    → AIScript
    listAssets(AssetType) → vector<string>
```

### Mod Loader

```
engine/content/ModLoader.cpp
    scanModsDirectory("mods/")
    loadManifest("mods/<name>/manifest.toml")
    buildContentStack()   — sorted by priority; higher-priority packs override lower
    resolveAsset(name, type) — walk stack until found
```

### Mod Manifest Format (TOML)

```toml
[mod]
name        = "Example Content Pack"
id          = "example-content"
version     = "1.0.0"
engine-api  = "1.0"
priority    = 100
depends     = []
```

### Content Pack Layout on Disk

```
mods/
    example-content/          ← compiled content pack (ships as shared library)
        manifest.toml
        example-content.dll/.so
    fl-base-pack/             ← bundled default content (Phase 2+, jomkz/fl-base-pack)
        manifest.toml
        aircraft/
        missions/
        terrain/
        audio/
    my-reskin-mod/            ← user mod example
        manifest.toml
        aircraft/f22/
            F22.png
```

**TOML vs YAML:** TOML is used for definition and configuration data (flight models, weapon specs, unit data, mod manifests, HUD layouts, playlists). These files have fixed schemas, typed values, and benefit from TOML's parse-time type enforcement and clean Git diffs. YAML is used for mission and campaign files, which are document-like: arbitrary nesting depth, large object lists, and YAML anchors/aliases let shared definitions be referenced multiple times without repetition. The rule of thumb is: does this look like a settings file (TOML) or a scenario/narrative document (YAML)?

## Repository Naming Convention

All first-party repositories and binaries in the fighters-legacy ecosystem use the `fl-` prefix. Content pack plugins for specific external games use a `<game>-content` pattern. Core repositories keep their full names.

| Pattern | Examples |
|---|---|
| `fl-<name>` | `fl-server` (dedicated server), `fl-lobby` (matchmaking service), `fl-base-pack` (bundled default content) |
| `<game>-content` | `fa-content` (Jane's Fighters Anthology content plugin) |
| Full name | `fighters-legacy` (engine + game), `fighters-codex` (reference repo) |

---

## Key Design Constraints

- **Cross-platform from day one.** All code compiles on MSVC (Windows), GCC/Clang (Linux), and AppleClang (macOS). Platform-specific paths are confined to `platform/`.
- **`IContentPack` is the only content boundary.** Adding a new content source means implementing this interface, not modifying the engine.
- **C++20, no extensions.** `CMAKE_CXX_EXTENSIONS OFF` is enforced across all presets.
