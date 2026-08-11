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

The HAL exposes platform-independent interfaces to the engine core. Nothing above the HAL layer links directly against Vulkan, SDL3, OpenAL, or ENet headers — formalized and enforced at configure time; see [Module Boundary Policy](#module-boundary-policy) below.

For upstream documentation on each backend, see [`docs/developer/references.md`](references.md).

#### Platform HAL Interfaces

All interfaces live under `platform/` and are exposed via the `platform-hal` CMake INTERFACE library. Engine core and backends link against `platform-hal` rather than including headers by path. All platform HAL types (`IRenderer`, `ILogger`, `IInput`, `Key`, `HudElement`, etc.) are in `namespace fl`.

| Interface | Header | Purpose |
|---|---|---|
| `IWindowEventHandler` | `platform/IWindowEventHandler.h` | Callback target for window events (resize, close); implemented by the engine game loop |
| `IWindow` | `platform/IWindow.h` | Create/destroy OS window, pump events, query dimensions, expose native handle for surface creation; fullscreen toggle (`setFullscreen`), window resize (`setSize(w, h)`), display mode selection (`setDisplayMode`), title update (`setTitle`), current monitor query (`getCurrentMonitorId`). `width()`/`height()` return physical framebuffer pixels (GPU/swapchain resolution; ≥2× logical size on Retina/HiDPI); `logicalWidth()`/`logicalHeight()` return DPI-independent window size matching SDL pointer-event coordinates |
| `IDisplay` | `platform/IDisplay.h` | Monitor enumeration, fullscreen display mode listing, per-monitor refresh rate query (used by renderer for vsync decisions) |
| `ICursor` | `platform/ICursor.h` | OS cursor shape control: standard shapes (`Arrow`, `Hand`, `Crosshair`, `ResizeNS`, `ResizeEW`, `ResizeAll`, `Text`, `None`) and custom RGBA bitmap cursors |
| `IRenderer` | `platform/IRenderer.h` | Render frame lifecycle: init, beginFrame, endFrame, shutdown; `setOverlayLines` for debug text overlay; `submitOverlayElements(span<HudElement>)` for 2D game overlay elements (cockpit HUD, rain, notices — accumulated per frame, cleared by endFrame); `setConsoleElements(span<HudElement>)` for the game console overlay (non-owning span, cleared by endFrame) |
| `IAudio` | `platform/IAudio.h` | Buffer upload, source play/stop/position/velocity, listener transform/velocity, source-relative (non-positional) mode |
| `ITextInputHandler` | `platform/IInput.h` | Callback target for OS text input and IME composition events; implemented by any UI component that accepts free-form text |
| `IInput` | `platform/IInput.h` | Keyboard, mouse, and gamepad state (SDL3 GameController API); haptic feedback (`rumble`, `rumbleTriggers`, `stopRumble`, `supportsRumble`, `supportsTriggerRumble`); drives text input mode via `ITextInputHandler` |
| `IJoystickEventHandler` | `platform/IJoystick.h` | Callback target for joystick hot-plug (add/remove); implemented by any system that tracks HOTAS devices |
| `IJoystick` | `platform/IJoystick.h` | Raw joystick/HOTAS input: arbitrary axis count, hat switches, button state, device name + GUID for binding persistence; hot-plug events via `IJoystickEventHandler` |
| `INetworkEventHandler` | `platform/INetwork.h` | Callback target for network events (connect, disconnect, receive); implemented by the multiplayer subsystem |
| `INetwork` | `platform/INetwork.h` | UDP transport: bind/connect, send/recv, peer state, frame pump |
| `IFilesystem` | `platform/IFilesystem.h` | Synchronous file I/O and directory scan over two path domains (Assets, UserData) |
| `IAsyncFilesystemHandler` | `platform/IAsyncFilesystem.h` | Callback target for async read completions; implemented by the terrain streaming subsystem |
| `IAsyncFilesystem` | `platform/IAsyncFilesystem.h` | Non-blocking whole-file reads for per-frame terrain chunk streaming; completions dispatched via `service()` |
| `ILogger` | `platform/ILogger.h` | Structured logging routed to the platform-native output |

**Event handler pattern:** `IWindowEventHandler`, `INetworkEventHandler`, `ITextInputHandler`, `IAsyncFilesystemHandler`, and `IJoystickEventHandler` are separate interfaces registered with their respective `IWindow` / `INetwork` / `IInput` / `IAsyncFilesystem` / `IJoystick` instances. The engine implements the handler; the platform backend calls it during `pollEvents()` / `service()` / text input events / async I/O completions / hot-plug events. This keeps platform-to-engine callbacks decoupled without requiring the backend to know anything about the game loop.

**Wiring — `Platform` struct:** `platform/Platform.h` defines a plain aggregate struct holding `std::unique_ptr` to each interface. The platform entry point (e.g. `platform/sdl3/`) constructs a `Platform`, populates it with concrete backend instances, and passes it to the engine on startup. The engine holds `Platform` by value and owns all interface lifetimes. Backends can be mixed freely — a test build might use a null renderer stub alongside a real filesystem backend.

**Design rules for all HAL interfaces:**
- No platform-specific headers (`SDL3`, `vulkan.h`, `al.h`, `enet.h`) may appear in any interface file.
- `IWindow::nativeHandle()` returns `void*` — the only platform type that crosses the boundary, and it does so as an opaque pointer used only by the Vulkan backend internally.
- All interface methods are pure virtual. No implementation code lives in these headers.
- Interfaces that can fail during init expose `getLastError() const → const char*` for human-readable diagnostics.
- **Thread safety:** `ILogger::log` is the only HAL method guaranteed thread-safe. All other methods on all other interfaces must be called from the main thread.
- **`IAsyncFilesystem` threading note:** The background worker thread is an internal implementation detail of the SDL3 backend. All `IAsyncFilesystem` methods — including `service()`, `readFileAsync()`, and `cancelRead()` — must be called from the main thread. Completion data passed to `onReadComplete()` is valid only for the duration of the callback; callers must copy any bytes they need before returning.
- **`IJoystick::flush()` note:** Must be called once per frame alongside `IInput::flush()`, after all input has been read for that frame. Devices recognised as standard gamepads are owned exclusively by `IInput` (SDL_Gamepad); `IJoystick` (SDL_Joystick) handles only raw HOTAS devices that are not standard gamepads.

**`IRenderer` scope:** `IRenderer` provides retained GPU resource management (`createMesh`, `createTexture`, `createMaterial`, `destroy*`), per-frame scene submission (`setScene(FrameScene)`), and renderer settings (`applySettings(RendererSettings)`). The Vulkan backend (`VkRenderer`) runs, per frame: a particle compute dispatch, then shadow → opaque forward (PBR + CSM; writes HDR + a world-space normal G-buffer) → GTAO compute → sky → transparent → bloom → tonemap (which composites bloom + ambient occlusion and applies FXAA). The opaque/terrain path also does altitude/slope biome blending and, at Atmospheric sky quality, analytic aerial perspective. `RendererSettings` (defined in `RenderTypes.h`) carries vsync mode, AA mode (`Off`/`FXAA`/`TAA` — MSAA was removed in favour of TAA), bloom, draw distance, shadow quality, particle density, ambient-occlusion quality, sky quality, and auto-exposure — populated from `GraphicsSettings` in `main.cpp` so that `platform/` headers remain free of `engine/` dependencies. See [rendering.md](rendering.md) for the detailed pass-by-pass render graph, the GPU resource layouts and the camera-relative invariant.

**`IFilesystem` is synchronous:** `readFile` blocks until the OS delivers the data. It is correct for startup asset loading, mod discovery, and config reads. It must not be called on the main thread for per-frame terrain streaming. See `IAsyncFilesystem` for async terrain streaming reads.

### Engine Core (`engine/`)

Game logic and simulation, independent of any specific content:

- **Game loop** (`engine/loop/`) — `TimeController` (fixed-timestep accumulator, time compression), `GameLoop` (sim thread, frame gating, render alpha). `ISimUpdate` is the callback interface for game systems advancing each tick. `GameLoop::enqueueSimCallback()` allows any thread to queue a one-shot lambda that runs at the top of the next sim tick — used by the game console to dispatch entity mutations safely.
- **Entity system** — component-based scene graph
- **Flight model** — aerodynamics simulation
- **AI runtime** — C++ autopilot controllers (`engine/ai/`) and Lua-scripted AI behaviours
- **Mission loader** — scenario and campaign structure
- **Localization** — keyed string lookup with BCP 47 fallback chain, `{placeholder}` interpolation, plural forms, and RTL metadata; see `engine/i18n/`

### Content System (`engine/content/`)

Bridges the engine core to external content:

- **`IContentPack`** — the single interface all content packs must implement. Exposes asset loading, mission data, configuration, and security metadata (`getTrustLevel`, `isNativePlugin`). The engine calls only this interface; it never knows what implements it.
- **`AssetManager`** — caches and serves assets via active content packs. Runs `AssetValidator` on every returned asset (magic-byte checks + size limits) before caching; discards and logs any asset that fails.
- **`ModLoader`** — discovers directory mods and compiled plugins at runtime. Validates `id`/`name` manifest fields against path-traversal, Windows reserved names, and length limits. Parses optional `[mod.trust]` section into `TrustLevel`. Detects native plugin binaries and fires `IContentPackEventHandler` callbacks. Loads plugins with `RTLD_LOCAL | RTLD_NOW` (POSIX) or full-path `LoadLibrary` (Windows) to prevent DLL planting.
- **`LuaSandbox`** (`engine/script/`) — sandboxed Lua 5.5 execution environment for AI and mission scripts. Deny-lists `io`, `os`, `package`, `debug`, `dofile`, `loadfile`; replaces `require` with a custom loader restricted to the pack's own `ai/` directory. Rejects precompiled bytecode. RAII destructor calls `lua_close()`.

  **Lua is compiled as C++, and that is a correctness requirement rather than a preference (#1015).** Lua raises an error by unwinding from the point of failure back to the enclosing `lua_pcall`. Built as C it does that with `longjmp`, which cannot safely cross a C++ frame: MSVC implements `longjmp` by *unwinding* the frames it passes, so a `lua_CFunction` written in C++ has its destructor funclets run against an EH state that no longer describes live objects — an access violation inside `~ifstream`, which is how #1015 presented. Itanium-ABI platforms fail the other way, skipping destructors and leaking. The alternative to changing the build mode is a rule — *never hold a C++ object when Lua may raise* — that no compiler checks, that every one of the 22 registered functions in `LuaController.cpp` must independently honour, and that `LuaSandbox.cpp` was already violating while carrying a comment claiming otherwise. Compiled as C++, `ldo.c` selects `throw`/`catch` (upstream's documented first choice) and the rule disappears. Consequences: there is no system-Lua path in `cmake/dependencies.cmake`, and translation units including `<lua.h>` must not wrap it in `extern "C"` (nor use `lua.hpp`, which is such a wrapper).

Mods can ship translations by placing TOML files under `locale/<lang>/` inside their mod directory. The `Localization` system merges these with the engine's base `locale/` files; higher-priority mods win on key conflicts. See [docs/modding/localization.md](../modding/localization.md).

### Content Packs (external)

Any repository that implements `IContentPack` and compiles to a shared library can be loaded as a content pack. The engine is indifferent to the source or format of the underlying assets.

## Module Boundary Policy

The monorepo is kept **split-ready** (2026-06-28 decision record): a future
`fl-engine`/`fl-client`/`fl-server` repo split (post-protocol-freeze) must be nearly free. The
layered model above is therefore not just a drawing — it is a set of CMake-target-level link rules
asserted on every configure. At the target level the layer DAG is:

```
  fighters-legacy (game client)              fl-server (headless server)
      │  │  │                                     │
      │  │  └── client backends:                  │  (no client backend may
      │  │      platform-sdl3 / platform-vulkan / │   reach fl-server)
      │  │      platform-openal                   │
      │  └───────────┬────────────────────────────┘
      │              ▼
      │      platform-net (createNetwork() facade)
      │              ├── platform-enet  (enet6)
      │              └── platform-gns   (GameNetworkingSockets, FL_ENABLE_GNS)
      ▼
  engine-* ──────────────▶ platform-hal (INTERFACE: HAL headers + GLM)
      │                        headless platform utilities also allowed:
      │                        platform-file-logger / platform-subprocess / platform-stdfs
      ▼
  engine-protocol (zero-dep wire-protocol seam: stdlib only)
      ▲
      └── consumed by engine-net, the game client, and the headless tools
          (bot_swarm, net_check) without pulling WorldBroadcaster / engine-entity
```

**Allowed edges (policy):**

- **`engine-*` → `platform-hal` is the one allowed platform edge** for backends: `platform-hal` is an
  INTERFACE target carrying only the HAL headers and GLM. Engine targets may additionally link engine
  siblings, the pure-stdlib headless platform utilities (`platform-file-logger` — the existing
  `engine-crash` edge — `platform-subprocess`, `platform-stdfs`), and vetted header/static third-party
  libraries (tomlplusplus, Lua, stb). They may **never** reach a client backend (`platform-sdl3`,
  `platform-vulkan`, `platform-openal`) or a transport backend (`platform-enet`, `platform-gns`,
  `platform-net`), nor the third-party libraries behind them (SDL3, Vulkan, OpenAL, enet6,
  GameNetworkingSockets). Concrete backends are constructed in the binaries (`main.cpp`/`Game.cpp`)
  and injected through the HAL interfaces.
- **`engine-protocol` reaches only the C++ stdlib** (`Threads::Threads` permitted as the stdlib's
  threading facility). This is the seam a repo split would cut: the wire types + codec are consumable
  by the server sim, the game client, and headless tools without transitively pulling the server sim
  or the entity system.
- **`fl-server` links no client backend** — headless means headless: no SDL3, Vulkan, or OpenAL,
  statically or dynamically. Transport backends are legitimate server dependencies.
- **Transport backends are reached only through the `platform-net` facade**: the game binary and
  `fl-server` link `platform-net` and select a backend at runtime via `createNetwork(TransportKind)`;
  neither may link `platform-enet`/`platform-gns` directly (the #507 HAL-leak fix, kept fixed). The
  load tools (`net_check`, `bot_swarm`) deliberately link `platform-enet` as a regression instrument
  and are exempt.

**Enforcement mechanics:**

- **Configure time** — `fl_assert_layering()` in [`cmake/layering.cmake`](https://github.com/fighters-legacy/fighters-legacy/blob/main/cmake/layering.cmake),
  armed from the root `CMakeLists.txt` via `cmake_language(DEFER CALL ...)` so it runs after every
  `add_subdirectory()` (all targets exist, including conditional ones). It walks
  `LINK_LIBRARIES`/`INTERFACE_LINK_LIBRARIES` transitively (aliases resolved, `$<LINK_ONLY:...>`
  stripped) and fails the configure with the full offending link chain
  (e.g. `engine-world -> platform-sdl3`) for any of the four rule classes above. The walker
  self-tests against synthetic targets on every configure, so a refactor that silently broke the
  graph walk fails configure instead of disabling enforcement.
- **CI binary backstop** — the "Verify static linking (Linux)" step in `ci.yml` runs `ldd` on both
  the game binary (rejects dynamic `sdl3|openal|ktx`; our bundled libs must be static) and
  `fl-server` (rejects `sdl3|vulkan|openal`; no client backend at all).

## Locked Architectural Decisions

These decisions are finalized and not subject to revision without an RFC — or, during primary
development (pre-`kProtocolVersion` freeze), a dated **decision record** (see below). The
2026-06-28 re-target to 128+ multiplayer revised several rows via that lightweight path.

| Concern | Choice | Rationale |
|---|---|---|
| Rendering | Vulkan + MoltenVK (primary); OpenGL 4.1 Core (Phase 3, Linux + Windows only) | MoltenVK → Metal on Apple Silicon. macOS uses Vulkan/MoltenVK exclusively — OpenGL is deprecated on macOS 14+ and is not a supported renderer path on Apple platforms. |
| GPU math | GLM (MIT) | INTERFACE dep on `platform-hal`; available everywhere without Vulkan |
| GPU memory | VulkanMemoryAllocator (MIT) | VMA v3.3.0; device-local staging for meshes/textures |
| Texture runtime | KTX-Software / Basis Universal | Apache-2.0; transcode at load → BC7 desktop, ASTC Apple Silicon |
| World coordinate convention | Right-handed Y-up meters (glTF-native) | No axis conversion needed; Vulkan Y-flip handled in projection matrix |
| Depth convention | Reverse-Z (near=1 far→0, compare=GREATER, D32_SFLOAT) | Better floating-point precision distribution across scene depth |
| Windowing / input | SDL3 | Wayland + modern controller support; long-term path |
| Audio | OpenAL Soft | Positional 3D audio; native music in OGG; no MIDI dependency in engine core |
| Network transport | **GameNetworkingSockets** (BSD-3, v1.6.0) for 128+ (Epic L, #506); **enet6** v6.1.3 retained as the LAN/single-player/low-count backend. **Both landed in-tree** (#507/#508/#509) behind `createNetwork(TransportKind)`; `fl-server` selects via `[network] transport`, the game client uses GNS for internet MP / enet6 for single-player. | enet6 was sized for ~32 peers and ships no encryption. GNS is selected for encryption (curve25519+AES-GCM) + mature congestion control + connection headroom (not throughput — #505 showed transport is not the ceiling). Both live behind the `INetwork` HAL via a backend-selecting factory (`-DFL_ENABLE_GNS`), so engine/game code is backend-agnostic and lean enet6-only builds stay protobuf-free. Crypto backend = **OpenSSL** and protobuf is **system-preferred** (both reversed the #506 spike during implementation — GNS rejects libsodium AES on ARM, and CMake blocks the pure-FetchContent protobuf handoff). See [docs/developer/gns-backend.md](gns-backend.md) + [docs/developer/decisions/transport-selection.md](decisions/transport-selection.md). (Revised by the 2026-06-28, 2026-07-01 decision records + #507 implementation.) |
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
| Native AI scripts | Lua 5.5 | Embeddable, sandbox-able, moddable |
| Multiplayer topology | `fl-server` dedicated binary + `fl-lobby` REST service | Server-authoritative; no P2P player-count cap; self-hostable |
| Multiplayer scale target | **128+ simultaneous players** (32 = near-term acceptance floor) | Drives the scaling seams below; see [docs/developer/design.md](design.md) "Multiplayer at Scale". (Revised by the 2026-06-28 decision record.) |
| Server simulation | Data-parallel **job system** over a single authoritative tick + a graceful **overrun governor** | Parallelizes per-entity integration + AI **and per-peer snapshot assembly** (workers build buffers, the sim thread flushes) so 128 players + AI + projectiles hold 60 Hz; when the tick still exceeds budget the `TickGovernor` sheds work (snapshot cadence/budget + AI-sample decimation) rather than spiralling, with the `GameLoop` catch-up cap as the bounded backstop. Spatial sharding deferred as a later option |
| Wire state encoding | **Quantized / bit-packed** snapshot stream (#515 ✓) + 3D interest culling (#402 ✓) + per-client priority/budget scheduling (#516 ✓) + client-acked delta baselines (#517 ✓) + selective-ack identity precision (#566 ✓) + adaptive per-client send-rate / congestion response (#518 ✓) | Quantized codec landed: frame-origin-relative positions, smallest-three quaternion, quantized vel/omega — replaced the fixed 64/88-byte records (see [docs/developer/decisions/snapshot-quantization.md](decisions/snapshot-quantization.md)). Priority/budget scheduling (relevance-ranked per-client byte budget + hybrid despawn/retention) keeps per-client bandwidth bounded as population grows. A per-peer AIMD `CongestionController` then sheds bandwidth on degraded links by decimating the snapshot send rate and scaling the byte budget, driven by ENet loss/RTT — see [docs/developer/decisions/congestion-control-design.md](decisions/congestion-control-design.md) |
| Player identity / auth | **Server-side, pluggable `IIdentityProvider`** (offline-verifiable signed tokens) | Persistent stats/ranking/bans key on a verified account, not a spoofable client GUID; self-hostable, no first-party hosted infra |
| Persistence | **`IPersistence` storage HAL** (SQLite single-server, Postgres for clusters) | Accounts, stats, bans, persistent-world state; promotes file-based banlists into a store |
| Cluster orchestration / live services | **Go** (k8s/OpenShift operator, Agones-native; `fl-account`, `fl-review`) | Intentional polyglot boundary: C++ engine/game/server, Go infrastructure; idiomatic for the k8s ecosystem |
| Single-player topology | `fl-server` running locally (`bind_address=127.0.0.1`, `max_peers=1`) | One simulation path; no bifurcated codebase; single-player is multiplayer with one peer |
| LAN server discovery | Raw UDP broadcast + IPv6 link-local multicast (#91) | `DiscoveryBeacon` (fl-server) + `DiscoveryListener` (game client) in `engine/net/`; separate socket outside ENet; client server browser in issue #143 |
| Weather and time of day | Server-authoritative `WeatherController` in `engine/weather/` | Autonomous cycle (Clear→PartlyCloudy→Overcast→Rain→Storm); `Snow` and `Blizzard` are operator-set presets that do not participate in the autonomous cycle; 10× time-scale default, wind/gust/turbulence model; synced via `MsgWeatherState` (0x04) at ~6 Hz |
| Entity system | Dynamic pool, no hard caps | No fixed object count limit |
| Sensing / detection | **One `SensorDef` vocabulary** (dual search/track lobes, per-check PoD at a 10 Hz reference cadence) in a new `engine/sensor/`; **`Contact` tracks are the only downstream representation** — avionics, AI, Lua and missile seekers never read ground truth. Signatures are unitless multipliers (radar range × `sqrt(sig)`; IR/visual linear); AI entities with no declared sensors get an implicit builtin eyeball. | One schema serves player avionics (#526), AI detection (#670) and seekers (#628/#676) instead of three drifting fragments. Routing every consumer through `Contact` makes "no omniscience" structural rather than a rule reviewers must remember — ground truth is not reachable from a consumer. (2026-07-12 decision record, Epic F #677/#678.) |
| Missile endgame resolution | **Probabilistic seeker, deterministic endgame**: missiles fly kinematically to impact; ALL probability lives in the seeker (PoD-gated acquisition/track through `Detection.h`, deterministic seeded dice; countermeasures modulate track-break/reacquisition); impact = proximity-fuze **geometry** → warhead. No terminal probability-of-kill table. | Resolves RFC #676. The honest-sensing framework landed after the RFC was filed and dissolves its dilemma: countermeasures are non-binary through the seeker rather than a Pk table, notching/kinematic defeat work as real geometry, one resolution path to test, and replay determinism comes free from the seeded dice. (2026-07-14 decision record, #583.) |
| License | GPL v3 | Engine modifications must stay open source; protects community investment |
| Hosting | GitHub, public repository | Unlimited Actions CI on public repos; GitHub Free sufficient |
| Async file I/O backend | Worker thread + `std::mutex` queue (`SDL3AsyncFilesystem`) | `SDL_AsyncIO` deferred; consistent cross-platform behaviour without conditional compilation in the interface |
| AI runtime / agentic services | **Pluggable OpenAI-compatible provider, local-first** (Ollama / llama.cpp reference); agents out-of-tick, validated-paths-only, MCP surface; **Go** `fl-director`/`fl-ops` services | Every AI feature degrades to scripted behaviour when no provider is configured (the CI-tested path — CI never requires a model); self-host posture preserved: no required cloud dependency, no first-party inference infra. See the 2026-07-01 decision record + [docs/developer/ai-architecture.md](ai-architecture.md) |

### Decision Records

During primary development (before the `kProtocolVersion` freeze) a locked decision may be
revised by a dated decision record instead of a full RFC, provided the change is recorded here
with its rationale. This keeps the velocity of pre-1.0 architecture work without leaving the
locked table silently stale.

**2026-07-30 — strategic architecture review: consolidation lands inside Phase 4, deployment
artifacts move forward, and the scale gate is made honest (plan #1036 Stages 5–9, epics #1063 /
#1064).** A full survey of the interfaces, the wire protocol, and the tracker against a 2026
128-player release, checked against code rather than against documentation. Ten decisions:

1. **The consolidation program lands inside M4.0, before the `v0.4.0` gate**, as new stages of
   #1036 with each stage shipping an interim `v0.3.14+` release. Phase 5 builds identity,
   persistence, anti-cheat, observability and a cluster operator directly on the server seams,
   and the review found those seams carrying enough debt that building on them would multiply it.
   Cleaning up afterwards is also strictly harder: the wire and the admin surface acquire external
   users at the gate. The **gameplay audit (#1065) runs last**, so it audits what actually ships.
2. **Two epics, not one.** Protocol hardening (#1063) and interface consolidation (#1064) map onto
   distinct components, distinct stages and distinct releases; a single 29-sub epic would have had
   a rollup nobody could read.
3. **Four hardening fixes join M4** — the rate-limit trio, connect-path fuzz seeds, `--persistent`
   stub removal, and the LAN-discovery port de-alias (the #1054 leftover). All four are live
   defects in released builds rather than design debt.
4. **fl-base-pack gating fixed at 13 issues.** #10, #11, #18, #44, #45 and — newly ruled — #42 are
   non-gating and re-homed under the successor content epic #1102, so **#54's sub-issue rollup
   equals gate truth** instead of needing mental adjustment while it is the number being watched.
5. **Deployment artifacts move Phase 7 → Phase 5** (#160, #228, #1096). The ordering was backwards
   for a self-host-only product: the Kubernetes operator that deploys the image sat a phase ahead
   of the image.
6. **New M5 gap issues** for what nothing tracked: admin-surface TLS (#1097 — no issue existed
   anywhere, while the MCP surface that can *act* rides the same plaintext listener), content-hash
   enforcement at join (#1098 — the wire field is already reserved and zero-filled), the
   sensor-driven relevance spike (#1099), transport-auth posture after identity (#1100), and
   `MsgClientInput` button widening (#1101 — bit 7 was the last free bit).
7. **`CLAUDE.md` (#1052) pulled to M4 and sequenced at the end of consolidation**, so the primary
   onboarding surface documents the post-consolidation architecture rather than being written
   twice.
8. **Declined:** auto-update (distribution channels own updates; the player-visible mismatch UX is
   the build-version work in #1074) and sensor-driven relevance as a Deferred Lever (the registry
   takes design-complete levers only, so it is a Spike until it is one).
9. **Two new Phase 4 acceptance lines** — the `reference` scale profile must pass with sensor
   content loaded and datalink plus voice active, and every client→server message type must be
   rate-limited or one-shot-gated with no amplified reply. The first exists because the committed
   128-client numbers were measured against a hollow world: `bot_swarm` bots carry no sensor
   content, so contact tables are empty and the `O(P²)` datalink fusion cost the review found is
   invisible to CI. **A gate that cannot fail on the bug it exists to catch is not evidence.**
10. **Every phase gate now files the incoming phase's execution-plan Task.** #1036 is why M4 landed
    in three clean interim releases; M5.0 currently holds ~70 open issues with no equivalent.

Architecture decisions D10–D23 arising from this review are recorded in #1036 and mirrored here
individually by the stage PRs that implement them.

**2026-08-04 — D18: a format carries a checked version iff it crosses a machine or build boundary
(#1068).** Six on-disk/on-wire formats each argued their versioning policy individually in a header
comment, reaching three different answers by three different routes. The rule they were all
circling: what makes a version field *load-bearing* is the possibility of the producer and consumer
disagreeing — which happens exactly when the format crosses a **machine boundary** (two independently
built peers meet at runtime) or a **build boundary** (the artifact outlives the build that wrote it).
A format whose producer and consumer always ship in the same tree cannot disagree with itself; its
version field would be a door nobody checks, so it freezes the number (or carries none) and relies on
additive-only evolution. Concretely:

| Format | Boundary crossed | Policy |
|---|---|---|
| Wire protocol (`kProtocolVersion`) | machine | checked; mismatch disconnects (frozen at 1 until the Phase 6 freeze) |
| `.flrep` replay (`kReplayFormatMajor/Minor`) | build — players download strangers' files | checked; newer major refused, minor skipped |
| `bindings.toml` (`InputBindings::kFormatVersion`) | build — a user file outlives installs | checked; older versions migrated, never silently reset |
| FLIT input trace (`InputTraceFormat.h`) | none — producer + consumer land together | frozen at 1 |
| `ServerTickReport` JSON (`kServerTickSchemaVersion`) | none — consumed by same-tree tooling | frozen at 6 |
| `WorldStateJson` | none — in-process consumers | deliberately versionless |

The six headers cite this record instead of re-deriving the argument. A NEW format answers one
question — who consumes this, built from what — and takes the row that falls out; it does not get a
version field "to be safe" (an unchecked version is how the pre-#932 release audit found tags whose
notes described a different version's contents: ritual metadata rots).

**2026-07-27 — player-facing AI content policy, RATIFIED (#932, RFC resolved).** Player sentiment
toward generative AI in games is strongly negative (mid-2026 surveys ~85 % below neutral). A project
that ships an agentic-AI initiative into that without a stated position invites every player to
assume the worst thing they can imagine, and the assumption compounds because it cannot be disproven
after the fact. So the position is stated, dated, and narrow enough to be verifiable:

1. **Shipped creative assets are human-authored or CC0, never generative output.** Art, music, story
   campaigns and voice packs. fl-base-pack `music/`, and the campaigns in fighters-legacy/fl-base-pack#3
   / #14 / #15, stay human-authored. This is the line the project does not cross, and it is the one
   players actually care about — a bought or downloaded pack is a thing they keep.
2. **Runtime-generated ephemeral content is permitted, opt-in per server AND per client, labeled,
   and never baked back into a shipped pack.** Briefings, debriefs, radio chatter, TTS speech.
   Ephemeral is the load-bearing word: it exists for one session and is not an asset.
3. **Servers advertise which AI features are enabled** — `/status` and the server browser — so a
   player chooses with the information in front of them rather than discovering it mid-match.
4. **Contributors declare asset provenance in PRs** (a CONTRIBUTING rule in both repos).

Nothing built in Stages 1–3 violates this: every AI feature is opt-in and off by default, everything
generated is per-session, and the CI-tested path is the one with no model at all. The near-collision
to keep clean is #614/#013, TTS versus a human voice pack — a voice PACK is a shipped creative asset
under (1), while TTS speech synthesised at runtime is ephemeral under (2), and the distinction is the
artifact's lifetime rather than how it sounded.

Ratifying costs the project a capability it might otherwise have wanted later: generated art or music
in a shipped pack is now off the table without a new decision record superseding this one. That is
the point of writing it down.

**2026-07-27 — a replay file is versioned for real, and the world it describes is fingerprinted by
its quantized state, not its floats (#643/#644, plan #1036 D6/D8).** `.flrep` is the first artifact
this engine produces that OUTLIVES THE BUILD THAT WROTE IT, so it does not inherit FLIT's frozen
`version = 1`: every section is length-prefixed and skippable, an additive change is a MINOR bump a
reader tolerates, and a change of meaning is a MAJOR bump a reader REFUSES by name ("this replay is
format 3.0; this build reads format 1.x"). A silent partial read that renders a plausible wrong
flight is worse than a refusal nobody can miss. The header stores what cannot be re-derived later --
notably the session's planet radius, without which every geodetic readout on playback, and every ACMI
line exported from it (#923), is quietly wrong rather than absent.

The recording reuses the wire's own quantized records (`SnapshotCodec`) rather than a second
serialization, and the recorder taps the encode-once pass instead of re-quantizing, so there is one
implementation of what 0.125 m means. Playback publishes through the same `SimRenderBridge` a network
client publishes through (D7), which is why the renderer, the cameras and the HUD needed no
replay-awareness at all.

`ReplayStateHash` is the determinism primitive the codebase lacked: a per-tick FNV-1a fold over the
**quantized integer domain**, which removes the float-ordering ambiguity that would make a
cross-worker comparison flaky (the `rollPasses` reasoning applied to whole-world state). It carries
one contract, learned the hard way: **hashes are taken on DECODED entities, on both sides of any
comparison.** Smallest-three orientation drops the largest-magnitude component, so a rotation whose
two largest components are nearly equal has that choice tip when quantized -- the same world then
hashes two ways depending on which side of the codec the value came from, and no amount of
re-normalizing settles a tie that sits exactly on the boundary.

**2026-07-23 — server authority is a capability bitmask, and the game-master map is the first consumer
of one aggregated world-state surface (#944/#861/#600).** The single all-or-nothing `operator_password`
gate is replaced by a per-command capability check that knows *who* issued a command: `CommandRegistry`
carries a required-capability mask per command (unannotated = Admin-only, preserving today's
semantics), `dispatch(line, CommandIssuer)` refuses when the issuer lacks it, and `WorldBroadcaster`
builds the issuer from either the operator password (grants Admin — rung 1, the CI path) or a peer's
granted `PeerAuthority` caps (the empty-token *grant channel* — rung 2). This is the same enforcement
point the Epic M agentic-AI admin/MCP allowlist will construct its issuer against — one allowlist for
humans and agents. `PeerRole` (Pilot/Observer) stays embodiment-only and orthogonal to authority. The
**game-master overview map (#861)** consumes the **#600 aggregated world-state snapshot** — a
deterministic, ~1 Hz sim-thread copy of the whole battlespace — NOT per-camera `queryRadius` interest,
which would flood at 128 players; that snapshot is the surface designed *once* for both the GM map and
Epic M's world-state read API. The GM feed (`MsgGmWorldState`, unicast only to `gm_map`-capable peers)
and the map's select→order actions flow through the capability-gated command path, so there is no
second ungated authority surface.

**2026-07-22 — secondary render views extend `FrameScene` as POD fields, never new `IRenderer` pure
virtuals (#695, Epic #587).** The renderer was strictly one `CameraView` per frame; the target-slaved
inset (#698) needs the frame's scene rendered a second time from another camera into a sub-rect.
Rather than add an `IRenderer` method (which every backend + `MockRenderer` would have to implement),
`FrameScene` gains three POD fields — `insetEnabled`, `insetCamera`, `insetRect[4]` — so mocks and
every `setScene` consumer compile unchanged and the disabled path is bit-identical to before.
`VkRenderer` renders the inset as a scissored second forward pass into the HDR target (after the
transparent pass, before the HDR→sampled barrier) with its own per-frame camera UBO/descriptor set.
The camera-relative rebase invariant is preserved by composing the inset view with
`translate(mainOrigin − insetOrigin)` in double precision — `RenderItem` transforms stay
main-origin-relative, so `planetCenter` in the inset UBO is the MAIN camera's rebased value. Sky,
transparents, and a per-inset GTAO recompute are v1 out-of-scope (documented in code). This is the
generic capability; rear-view / missile-cam / MFD repeaters reuse it.

**2026-07-22 — client-side IFF stays client-side; the faction relationship matrix never crosses
the wire (#688, Epic #587).** The client already receives an entity's `factionIndex` (#860) and a
datalink track's server-computed `ident` (#528), but had no way to answer friend/foe for an
*arbitrary* snapshot entity (needed for the #641 combat-HUD target box and #696 target cycling).
Rather than send the `FactionRegistry` relationship matrix — it is Lua-mutable at runtime, so any
wire snapshot goes stale, and `RadarView`'s doctrine already says the display-safe fact is `ident`,
not raw faction — the client derives IFF locally: `ClientNetEventHandler::identForEntity` compares
the entity's faction to `ownFactionIndex()` (same → Friend), else uses a live datalink track's
`ident` (authoritative, catches coalitions), else the affiliation-rule fallback
`areFactionsHostile` (`engine/world/FactionDef.h`). No wire change; `kProtocolVersion` stays 1.

**2026-07-22 — terrain line-of-sight is a shared `engine/spatial` utility, decoupled from the
collision phase (#687, Epic #587 padlock family).** Nothing ray- or LOS-shaped existed in the
engine; the padlock lock-break (#697, client) and server-side AI sensing terrain gates (#670) both
need the same "can A see B over the terrain?" answer. Rather than fold it into the #630 collision
broadphase (whose scope is SpatialIndex range queries for ramming damage — no ray path), LOS lands
as a standalone header-only `engine/spatial/LineOfSight.h`: `terrainLos(a, b, heightFn, readyFn,
R, stepM, clearanceM) -> {Clear, Blocked, Unknown}`. The heightfield is an **injected callable**
(template, not `std::function`), so the client passes `TerrainStreamer::heightAt` and the server its
own heightfield — `engine-spatial` keeps its pure-stdlib, glm-free, dependency-free discipline
(endpoints are `double[3]` like SpatialIndex's positions; glm callers bridge with `glm::value_ptr`).
An adaptive, margin-bounded march densifies at grazing tangents and stretches over valleys without
tunnelling thin ridges; `Unknown` (returned when a tile is unloaded mid-segment) is caller policy —
padlock treats it as Clear so unloaded terrain never false-breaks a lock.

**2026-07-19 — ATC as a deterministic `engine-atc` library + a shared radio channel (Epic #673).**
Airports build *geometry* (#486/#487); ATC adds *behaviour*: runway sequencing, clearances, and a
player comms menu. The core decisions: **(1)** ATC is a new **`engine-atc`** static library
(`namespace fl::atc`) sitting above `engine-entity` + `engine-ai` (like `engine-script`), a **pure
deterministic FSM with no model involvement** anywhere — this is the CI-tested path
(docs/developer/ai-architecture.md); the AI-voice ATC epic (Epic O, #591) is a *voicing* layer over the same
enumerable `AtcPhrase` vocabulary, never a re-implementation. `engine-net` PRIVATE-links it and ticks
it at 1 Hz in the serial Maintenance phase. **(2)** A single generic **player radio channel**
(`MsgRadioCommand`/`MsgRadioTransmission`) carries ATC now and is reused by the #610 wingman grammar
later; it is verb-routed like the admin channel (validated grammar, never direct state mutation).
**(3)** Facilities are built **lazily** from the `AirportRegistry`, so an 80k-airport registry costs
nothing until traffic appears. **(4)** A `FacilityPose` provider (`std::function`) means a moving
carrier deck (#38) plugs into the identical logic — the FA "accepts landings is a data property"
lesson. Ground handling (#700: wheel brakes, rolling resistance, nosewheel steering) was load-bearing
prerequisite work — the landing half of ATC is impossible without a lander that can stop on the
runway. See #700–#706.

**2026-08-11 — the facility owns the flight lifecycle end to end (#1149).** The runway is a mutex and
arrivals win it over departures, so whoever retires a flight decides whether the field ever reopens.
That is the facility, not the caller: an arrival leaves the sequence exactly three ways — it lands
(down and stopped → `ClearanceState::Landed`, retired by `update()` and told to taxi to parking), its
entity dies, or the pilot cancels. Having the AI arrival composition report the landing instead was
rejected, because a human who lands and parks would then hold the runway for the rest of the session
unless every client remembered to do the same. `Landed` and `Departed` are **terminal**: they persist
so a caller polling `clearanceState()` can act on them, and a fresh `requestTakeoff()` clears them for
the turnaround. The occupancy timeout stays a pure deadlock backstop — it frees the runway but never
claims a landing the aircraft did not make.

**2026-07-18 — Multi-crew aircraft: seats unified into `ControlledEntity` (Epic #966).** Aircraft
gain 2+ crew positions ("seats") alongside the single-pilot model — a bomber with a pilot and
defensive gunners, spawned with every seat bot-filled, any non-human seat human-joinable. The core
decision: **crew seats are a `CrewState` (vector of `CrewSeat`) on `ControlledEntity`, not child
entities and not a controller facade.** `seats[0]` is the pilot and *is* today's controller path —
a plain fighter is the implicit 1-seat case with the single-seat path byte-for-byte unchanged.
Two alternatives were pressure-tested and rejected: (a) crew positions as child entities breaks the
edge-free data-parallel tick contract and forces "not-an-aircraft" filters across ~7 world systems
(spatial, sensing, collision, interest, scheduler, formations, kill-feed) while *still* needing a
composition seam to ferry pilot output to the parent integrator; (b) a `CrewController :
IEntityController` facade cannot express per-seat fire, because `sample()` returns a single
`ControlInput` and the weapons pass reads only `ce.lastInput`.

- **Capability partition, not a role enum.** A seat's role is a display string (`"pilot"`,
  `"tail-gunner"`); the engine and wire see a `CrewCapabilityMask` (uint16): `Fly`, `Fire` (+ a
  bound hardpoint-station list), `Radar`, `Countermeasures`, `Command` (the #591 seam). Mirrors the
  #944 roles-as-data decision and the "`allowed` IS the contract" hardpoint precedent.
  `validate-entity` enforces **one owner per control channel** (exactly one `Fly` seat; each
  hardpoint fired by at most one seat; `Radar`/`Countermeasures` at most one seat) — merge conflicts
  are impossible by construction.
- **Server-authoritative turrets with a directional launch vector.** A weapon station gains an
  aiming direction independent of the airframe nose: a `TurretState` slewed by a pure, rate-limited
  `stepTurret()` (unit-testable, reused verbatim as the client turret predictor), and a `FireRequest`
  world-space launch direction that `ProjectileSystem`/hitscan fire along. This **is** the ground-SAM
  launcher-elevation gap documented as owed to #585 — a static emplacement mounts its launcher as a
  turret and slews it via this servo.
- **Tick composition** is a masked merge, no aggregator: the AI pass iterates seats in fixed order
  inside the entity's worker slice (writes stay disjoint per entity) — a human seat reads its peer's
  drained input slice, a bot seat samples an `ISeatController` (narrower than `IEntityController` — a
  seat bot doesn't fly), an empty seat contributes nothing; the effective `ControlInput` takes each
  channel from its owning seat. The integrate pass steps turrets. The serial weapons pass loops
  seats; `FireState` splits into a shared per-entity `LoadoutState` (physical stations/ammo truth)
  plus a per-seat `FireChannel`, and `FireRequest`'s sort key becomes `(shooterIdx, seat, station)`.
  Seat-bot dice hash `(entityIdx, seatIdx, tick)` so the pass stays serial-equivalent (TSan-gated).
- **Wire** (additive, `kProtocolVersion` stays 1): `MsgCrewRoster` (0x13, reliable, on connect and
  on occupancy change); `MsgSeatRequest`/`MsgSeatResult` (0x14/0x15); a ConnectRequest seat-claim TLV
  (reserved 0x0500 range) for join-at-connect; `MsgConnectAck` gains a seat/role field. Crew peers
  send the unmodified 80-byte `MsgClientInput`; the server routes by the `{entity, seat}` binding and
  **masks channels by the seat's capabilities server-side** (a gunner's elevator is ignored, never
  trusted). Gunner aim reuses `viewAxis` (latest-wins, unbuffered — the turret servo is the low-pass
  filter). Snapshot: `isOwn` generalizes to "occupies a seat in this entity" so every human crew
  member gets the omega + loadout own record; an optional crew block (occupancy + quantized turret
  az/el) is gated like `hasOmega` and emitted only when `seats.size() > 1`, so single-seat byte-shape
  tests stay bit-identical.
- **Three-state occupancy** `{Bot | Human(peerId) | Empty}` with `kNoPeer = 0xFFFFFFFF` (the
  `Formation.h` "never use 0" rule); unmanned human-joinable seats are legal from day one. **Seat
  state lives on the seat, not the controller**, so vacate/disconnect reverts the seat to its authored
  default by a pointer flip — a parked bot pilot resumes mid-maneuver with intact `FireChannel`/ammo/
  turret pose. Humans never displace humans; the airframe dies only when the owning pilot leaves an
  otherwise human-free, peer-spawned aircraft (never yank an airframe out from under a live gunner).
- **Per-instance skill** — the first per-instance skill in the engine (today `AiTuning` is type-level
  and sensing-only). Rolled per seat from a mission `[min, max]` with a deterministic seed (mission
  seed ⊕ object id ⊕ seat index, the `detectionHash` integer-RNG idiom); it plumbs to sensing (the
  `Radar` seat's rolled skill) and finally gives `AiScaling::aimErrorDeg` a consumer in the gunner/AAA
  fire path. Provable zero-pack via a compiled-in `builtin:bomber` (pilot + tail turret) in the armed
  sandbox. Full decomposition on Epic #966; wire tables land with the netcode wave in
  [network-protocol.md](network-protocol.md).

**2026-07-17 — VR (OpenXR) deferred to Phase 8, not rejected.** A mid-2026 gap analysis against the
genre bar noted that VR is a celebrated feature of the closest comparable title (Project Wingman)
and a real differentiator for this niche — but it is a renderer-architecture concern (stereo
swapchains, per-eye passes, reprojection timing, cockpit-scale world-space UI) that belongs with
Phase 8's "Rendering & Alternative Backends", not earlier. It is therefore **deferred to a Phase 8
epic, not dropped**. The one obligation this places on Phases 4–7: **no new render pass or HUD/UI
layer may bake in a single mono eye/camera assumption** — HUD/cockpit layers keep a
world-space-renderable path in mind, so VR is an additive backend rather than a rewrite when it
lands. Only **OpenXR** (Khronos, Apache-2.0, vendor-neutral) is acceptable — never a vendor SDK.
Head tracking (opentrack UDP, a trivial 6-DoF localhost stream) and force feedback are the cheaper
adjacent items and are scheduled independently (P6 view family / P8). Revisit at Phase 8 planning
with market data.

**2026-07-15 — Unified connect handshake (#853).** The client now sends `MsgConnectRequest` first
on connect (role, requested entity type, mounted-pack manifest, a reserved entitlement-token ext
block) and the server replies `MsgConnectAck` (granted role + assigned entity) or
`MsgConnectRefusal` — replacing the "server unilaterally spawns `builtin:debug-entity` on connect"
flow. The join flow was designed **once** so the observer role (#857), player entity type (#834),
required-pack policy (#872), and future premium entitlement (RFC #871) each consume one message
instead of accreting independent TLVs onto `MsgConnectAck`. As part of it, the ENet `MsgId` space
was split: `0x00–0x1F` for ENet messages, `0x20+` for raw-UDP/non-ENet ids (`MsgLanBeacon` moved
`0x10 → 0x20`), freeing an id for `MsgConnectRequest` (`0x11`) — a deliberate raise of the boundary
the prior code comment flagged as "a choice, not an accident". Wire detail in
[network-protocol.md](network-protocol.md); both ends land together so `kProtocolVersion` stays 1.

**2026-06-28 — Re-target to 128+ multiplayer.** A roadmap gap-analysis against the bar of a
modern combat flight sim supporting 128+ simultaneous players found the architecture sized for
~32. The following decisions were revised (rows above updated accordingly):

- Multiplayer scale target **32+ → 128+** (32 becomes the near-term acceptance floor).
- Multiplayer is now a **co-equal product pillar** (PvP + co-op PvE + persistent world), not a
  secondary extension to single-player.
- New seams added as locked decisions: data-parallel **server job system**, **quantized wire
  state** + priority/budget scheduling, **server-side identity/auth**, **persistence HAL**, and
  a **Go** cluster operator + live-services tier (polyglot boundary).
- **Network transport** (`enet6`) reversed: under replacement for the 128-peer target
  (GameNetworkingSockets lean) behind the `INetwork` HAL. *(Selection settled 2026-07-01 — see the
  decision record below and [docs/developer/decisions/transport-selection.md](decisions/transport-selection.md).)*
- **Hosting model:** self-host only — the project ships the identity/live-services software but
  runs no central infrastructure; communities self-host. (No first-party PII/GDPR liability.)

These land across a new **Phase 5 — Multiplayer at Scale & Live Services** and scaling seams in
Phases 3–4; see [docs/roadmap.md](../roadmap.md).

**2026-06-28 — Server job system: data-parallel tick, not spatial sharding (Epic A, #510).** The
8-core Release reference-env characterisation (#505) showed `WorldBroadcaster::onTick` is the 128+
ceiling — CPU-bound on the single-threaded per-entity sim + per-peer snapshot build, not on
`enet6`. The chosen approach parallelises the per-entity work *within a single authoritative tick*
across a worker pool (`engine-job`), rather than sharding the world into multiple authoritative
regions. Rationale: per-entity work is already embarrassingly parallel (each entity owns its
`FlightIntegrator`; the `SpatialIndex` + controllers are read-only mid-tick), and one tick keeps a
single consistent world with no cross-shard hand-off or snapshot stitching. Both the per-entity
integration + AI passes (#511) **and the per-peer snapshot assembly** (#512) run data-parallel over
the same worker pool — for snapshots, workers build per-peer buffers (each peer's interest query +
scheduler + encode is `peerId`-isolated) and the sim thread flushes (`m_net.send` stays
sim-thread-owned). Spatial sharding is the deferred next scaling axis. Full design in
[docs/developer/decisions/server-job-system-design.md](decisions/server-job-system-design.md).

**2026-06-29 — Quantized snapshot encoding + 3D interest (Epic B, #515/#402).** The reference-env
characterisation (#505) measured ~480 KB/s/client downstream at 128 idle clients — 3.2× the
≤150 KB/s gate — driven by the fixed 64/88-byte per-entity snapshot records. The per-tick entity
body is now a quantized, bit-packed record stream (position relative to a per-snapshot `double`
frame origin, smallest-three quaternion, quantized velocity/omega; full-vs-delta per-record `full`
bit), and the per-peer interest query gained an exact 3D (XYZ) distance gate. Encoding-only and
transport-agnostic (stays on `enet6`), so it proceeds independently of the transport replacement
(Epic L). The priority/budget scheduler (#516) and client-acked baselines (#517) build on this codec;
congestion response (#518) follows. Full design in
[docs/developer/decisions/snapshot-quantization.md](decisions/snapshot-quantization.md).

**2026-06-29 — Priority/budget snapshot scheduler (Epic B, #516).** The quantized codec (#515) cut the
per-record cost but the server still sent *every* visible entity every tick, so aggregate bandwidth
still grew O(visible) per client. Each peer now gets a per-tick byte budget (`[world]
snapshot_budget_bytes`, default 1200, 0 = unlimited); the pure `engine/net/SnapshotScheduler` ranks
the visible set by relevance (distance, closing-speed, recency, player-owned) and sends only the
highest-priority records that fit, deferring the rest. A recency term guarantees eventual inclusion of
every visible entity (no starvation) and the peer's own entity is always sent (client-side prediction
needs it). Because budgeting omits entities from some snapshots, this also adds a **hybrid despawn
model**: the client retains entity state across snapshots and evicts only on an explicit
`SnapshotDespawn` TLV (a confirmed sim removal) or after a retention timeout (interest-out / lost
despawn); the server force-sends a full record on re-entry past that window so a returning entity stays
decodable. Wire-additive (new TLV tag only, `kProtocolVersion` unchanged); the bandwidth win is
measured by `bot_swarm`'s `downstream_kbs_per_client`. Adaptive send-rate/congestion (#518) builds
on this.

**2026-06-29 — Client-acked delta baselines (Epic B, #517).** The fixed `[world]
baseline_interval_ticks` re-sync cleared every peer's known-entity set on the same global tick,
re-sending *all* visible entities as full records at once — a synchronized cross-peer bandwidth spike
— and left a dropped full record undecodable for up to that interval (~2 s). The server now drives
full-vs-delta off what each client has actually acknowledged: clients already echo the last snapshot
tick they processed in `MsgClientInput`/`MsgHeartbeat`, and the server treats that `tickIndex` as the
snapshot **ack** (clamped to the present, kept as a monotonic high-water mark). An entity is re-sent
as a full record every tick until the peer acks the tick its full streak started on, then converges
to deltas; a scheduler deferral restarts the streak so an ack of a tick on which the entity was
withheld cannot falsely confirm it. Recovery is now ~1 RTT and per-entity rather than a periodic
all-entity spike, with **no wire-format change** — the client additionally ignores out-of-order
snapshots so its echoed tick stays monotonic. The per-entity `kSnapshotRetentionTicks` force-full
(interest-out re-entry) is retained as an ack-independent backstop, and the `m_peerKnownGens` map is
now pruned on that same window (the periodic baseline clear used to bound it). `baseline_interval_ticks`
and `WorldBroadcaster::setBaselineInterval` are removed.

**2026-06-30 — Selective-ack identity precision (Epic B, #566).** The #517 high-water-mark ack cannot
distinguish "the client decoded the full sent at tick S" from "the client received a later tick ≥ S but
missed S", leaving a narrow residual: a full dropped on ≥2 consecutive ticks, plus a scheduler deferral
on the first tick the client does receive, could briefly mis-confirm the identity so the next delta is
undecodable (self-healing via the retention force-full, but a visible flicker). The client→server ack is
now paired with a 32-bit **selective-ack bitmask** (`MsgClientInput`/`MsgHeartbeat` `ackMask`, TCP-SACK
style) reporting which recent ticks below the high-water mark the client actually **decoded**; the pure
`engine/net/AckWindow.h` helper lets the server confirm the *specific* `fullStreakTick` rather than a
high-water mark. This closes the residual at the root and retires the #517 "deferral guard" (a
scheduler-withheld entity is never sent that tick, so acking it can no longer confirm its earlier streak
start). No wire-size or protocol-version change — `ackMask` reuses the two messages' former reserved
padding. Truncated-but-received snapshots remain out of scope (a partially-decoded tick still sets its
ack bit); the per-client byte budget keeps snapshots within one MTU fragment and the retention force-full
is the backstop.

**2026-07-01 — Network transport selected: GameNetworkingSockets, `enet6` retained (Epic L, #506).**
The transport-selection spike (#506) evaluated GameNetworkingSockets vs a bespoke UDP layer vs
hardening `enet6`, against the `INetwork` HAL contract. **GameNetworkingSockets (BSD-3) is
selected** as the strategic backend; `enet6` (MIT) is **retained** behind a backend-selecting
factory for LAN / single-player / low-count servers. The selection is justified **not** on
throughput — #505 proved the 96–256 ceiling is the sim + snapshot build, not the transport — but on
the three things `enet6` cannot give the 128+ target: **transport encryption** (carries Epic C auth
tokens), **mature congestion control** (beneath the application-level #518 response), and
**connection-count headroom**. Bespoke UDP is rejected as a man-months reinvention of a solved,
BSD-3-licensed problem; the honest fallback if GNS's Protobuf FetchContent build proves untenable
across MSVC/Apple Clang is defer-and-harden (keep `enet6` + add a libsodium/DTLS shim). Retaining
`enet6` behind the factory keeps the heavy Protobuf/crypto chain **out of** the single-player, LAN,
and CI-tool build paths. Crypto backend = **libsodium** (ISC). No `INetwork` interface change is
anticipated (encryption is backend-internal; the identity token travels in-band). Wire format
(`kProtocolVersion = 1`) is unchanged. Implementation follows in #507 (backend + factory,
closing the `Game.cpp` direct-`ENetNetwork` HAL leak), #508 (encryption/DTLS + LAN-beacon/RCON
reconciliation), and #509 (FetchContent GNS + Protobuf + libsodium + CI). Full evaluation in
[docs/developer/decisions/transport-selection.md](decisions/transport-selection.md).
> **[2026-07 implementation annotation]** The crypto and protobuf choices in this record were
> **reversed during #507–#509**: crypto backend = **OpenSSL** (GNS rejects libsodium AES on ARM)
> and protobuf is **system-preferred with a FetchContent fallback** (CMake blocks the pure-
> FetchContent protobuf handoff). The [Locked Architectural Decisions](#locked-architectural-decisions)
> table row and the [transport-selection.md](decisions/transport-selection.md) banner carry the corrected decision;
> this dated record is left intact as history.

**2026-07-01 — Dynamic World & Agentic AI initiative: pluggable local-first inference (Epics M–P,
#589/#590/#591/#592).** A strategic re-plan found the roadmap covered no ML/LLM/agentic patterns at
all, while the architecture is ideally shaped for them: agents operate at human timescale *outside*
the 60 Hz authoritative tick, acting through paths that already validate their inputs. A second
cross-cutting initiative ("Dynamic World & Agentic AI", epic letters M–P continuing A–L) adds an
agentic campaign director (`fl-director`), conversational crew/GCI, and agentic server operations
(`fl-ops`), on these locked choices:

- **Provider seam:** any OpenAI-compatible HTTP endpoint; the reference deployment is **local**
  (Ollama / llama.cpp server; Metal on Apple Silicon, CUDA/Vulkan on Windows, CUDA/ROCm/Vulkan on
  Linux); vLLM/hosted APIs allowed, never required. No engine SDK dependency — a minimal client over
  `IHttpClient` (#490). The engine and services never manage the inference-server lifecycle
  (endpoint-only). Mirrors the pluggable `IIdentityProvider` pattern; preserves the self-host,
  no-first-party-infra posture.
- **Out-of-tick guarantee:** LLMs never run in the 60 Hz tick. Agents consume a ~1 Hz structured
  world-state snapshot + match event stream assembled off the sim thread; sim tick p99 must be
  unchanged with agents attached (validated under the Epic I load harness).
- **Validated-paths-only actuation:** agents act exclusively through the admin/MCP command surface
  (server-side allowlist, building on the #233 REST substrate), mission YAML gated by
  `validate-mission`, and the `AiControllerFactory`/`StateMachineController` behaviour grammar.
  Direct state-mutation APIs for agents are rejected by design. Every agent action is audit-logged
  and captured in match recordings (epic #588).
- **Graceful degradation is normative:** with no provider configured, every feature falls back to
  scripted behaviour (command menu, scripted/random campaign, canned calls, dashboards-only ops) —
  and that fallback is the CI-tested path. **CI never requires a model**; model-dependent metrics
  run in spike-produced eval harnesses on the reference environment.
- **Tiered ops autonomy:** observe → recommend → act-with-allowlist; the act tier is limited to an
  operator-configured command allowlist.
- **Security posture:** player chat/names are untrusted input to agents (prompt-injection
  hardening: templated prompts, schema-validated outputs, grammar allowlists); the MCP surface
  threat model coordinates with the Epic D anti-cheat threat model (#545).
- **New repos (Go):** `fl-director` (Epic N) and `fl-ops` (Epic P) — the same polyglot service
  boundary as `fl-account`/`fl-review`/`fl-operator`; created when their scaffold issues start.

Full design in [docs/developer/ai-architecture.md](ai-architecture.md); initiative sequencing in
[docs/roadmap.md](../roadmap.md).

**2026-07-01 — Phase 3 close-out re-scope.** With the scaling spine landed (Epics A/B/L complete,
Epic I residuals only), Phase 3 (due 2026-07-31) was re-scoped to close on the engine-systems work
it actually gates: the spherical-Earth epic (#468, untouched at 22 sub-issues) moved to Phase 4 —
it enables content, not engine seams — taking the biome-texture items (#446/#447) with it;
renderer-advancement items (#443–#445, #448–#454) moved to Phase 8 under epic #597, and the Phase 8
milestone was renamed **"Rendering & Alternative Backends"** (its stale "voice chat deferred from
Phase 2" note removed — voice is Epic J in Phase 4); avionics/gameplay orphans (#210, #438, #462,
#128, #322, #226, #358, #233, #163) re-homed into the new Phase 4 epics. Phase 3 retains the Epic
A/I residuals, fuzzers (#94), layering CI (#559), and #439. In the same pass the Phase 4–9 backlog
was fully epic-structured (new epics #583–#598, legacy umbrellas #33/#34/#42/#50 closed as
superseded).

**2026-07-08 — `engine-protocol` wire-protocol seam (#712, sub-task of #559).** The wire protocol —
naturally-aligned message types (`GameProtocol.h`), the bit-packed snapshot codec (`SnapshotCodec` +
`BitStream` + `Quantization`) and the selective-ack window (`AckWindow.h`) — is promoted to its own
**zero-dependency** static library `engine-protocol` (only `SnapshotCodec.cpp` is a compiled TU; the
rest are header-only). It reaches nothing but the C++ stdlib, so the game client, `fl-server`, and the
headless tools (`bot_swarm`, `net_check`) consume the protocol **without transitively pulling
`WorldBroadcaster` / `engine-entity`** — the clean seam a future `fl-engine`/`fl-client`/`fl-server`
split needs. The server-sim *policy* pieces (`SnapshotScheduler`, `CongestionController`,
`TickGovernor`) and the peer-management code (`WorldBroadcaster`, `JitterBuffer`, `NetworkUtils`) stay
in `engine-net`, which now PUBLIC-links `engine-protocol` (existing consumers unchanged, no
include-path change — the files stay in `engine/net/`). The zero-dep guarantee is enforced at CMake
configure time by `fl_assert_zero_dep()` in the new `cmake/layering.cmake`, seeding the layering-guard
work; the full engine/client/server layer-DAG enforcement + CI `ldd` extension land in #559's
remaining sub-tasks.

**2026-07-08 — `platform-stdfs` single filesystem backend; `fl-server` drops SDL3 (#711, sub-task of
#559).** The `IFilesystem`/`IAsyncFilesystem` HAL is now backed by one implementation,
`platform-stdfs` (`StdFilesystem` + `StdAsyncFilesystem` over `std::fstream`/`std::filesystem`), and
the old `SDL3Filesystem`/`SDL3AsyncFilesystem` backends are deleted. The headless `fl-server` linked
`platform-sdl3` **solely** for terrain heightmap I/O — the last engine/server → client-backend leak;
it now links `platform-stdfs` and pulls in no windowing library. The decision was to *unify* rather
than add a second parallel backend: the SDL3 filesystem backends held no SDL-specific path logic (roots
are resolved by callers and passed into the constructor), so a `std::filesystem` implementation is a
true drop-in. The GUI client uses `platform-stdfs` too and keeps SDL only for what it is uniquely good
at — path *resolution* (`SDL_GetBasePath`/`SDL_GetPrefPath` in `Game.cpp`) and windowing. Files are
opened from `std::filesystem::path` objects (not narrow strings) so UTF-8 paths stay correct on
Windows; the async worker-thread + `service()` swap-drain model is unchanged. Enforced by extending the
CI "Verify static linking" step to assert `ldd fl-server` shows no SDL3. The remaining #559 sub-task
grows `cmake/layering.cmake` into full layer-DAG enforcement.

**2026-07-09 — split-ready layering enforced (#713, closes #559).** The module boundaries the
2026-06-28 monorepo-vs-split decision committed to — most already true, the last violation fixed by
#711/#712 — are now asserted on every configure instead of relying on review discipline. The seed
`fl_assert_zero_dep()` grew into `fl_assert_layering()` (`cmake/layering.cmake`, deferred from the
root `CMakeLists.txt`), which walks `LINK_LIBRARIES`/`INTERFACE_LINK_LIBRARIES` transitively and
fails configure with the offending link chain on four rule classes: (1) no `engine-*` target reaches
a client or transport backend; (2) `engine-protocol` links nothing beyond stdlib/`Threads::Threads`
(the direct `fl_assert_zero_dep` call folded in); (3) `fl-server` reaches no client backend; and
(4) — a deliberate scope addition beyond the #713 text — the game binary and `fl-server` link the
`platform-net` facade only, never `platform-enet`/`platform-gns` directly, locking in the #507
HAL-leak fix. The walker self-tests against synthetic targets each configure so a broken walk cannot
silently disable enforcement. The CI `ldd` backstop on `fl-server` broadened from `sdl3` to
`sdl3|vulkan|openal`. The full policy (layer DAG, allowed edges, enforcement mechanics) is documented
in the new "Module Boundary Policy" section above.

**2026-07-10 — Spatial sharding: deferred with an explicit trigger (Epic A, #572).** The
spatial-sharding spike (#572) — the contingent "next scaling axis" the #510 job-system decision
deferred — concludes **do not implement sharding now**, on three findings. (1) On a single machine,
sharding is *strictly dominated* by the data-parallel tick that already landed: shards and the
`JobSystem` consume the same cores, everything expensive (integrate, AI, per-peer serialize) is
already a `parallel_for`, and sharding re-partitions the same work while adding seam costs (ghost
halos, authority hand-off, migration) — a barriered-lockstep shard-threads design is functionally
the existing partitioning with extra steps. (2) The flight-sim workload is hostile to spatial
partitioning: the 200 km interest radius forces ~1000 km-class regions before the ghost halo stops
exceeding the owned area (a whole theater fits in one region), and combat *converges* — the furball
is a fully-connected interest graph in one region exactly at peak load. (3) What sharding uniquely
buys is **multi-machine** scale (shard-per-process + gateway + inter-shard bus + global entity IDs),
which is beyond the 128+ single-box target and is a Phase 5+ *product* decision that would ride
Epic K, not a performance contingency. Instead, a **pre-sharding ladder** attacks the measured
serialize bottleneck first: scale-up (worker headroom), shared snapshot quantization (#725 —
encode each entity once per tick instead of per peer; the per-peer `frameOrigin` is a chosen
constant, not a law), a governor interest-radius shedding lever (#726), LOD physics (#575) for the
integrate-bound case, and peer/transport sharding if the serial flush tail ever binds. An explicit **trigger criterion**
(sustained `load_factor` at floor + rising `dropped_ticks` + serialize-dominant tick at max workers
on the 8-core reference env, after the ladder lands — all observable from `ServerTickReport` v2 via
the #574/#569 outputs) defines when to revisit; a product decision to exceed one box triggers the
process-model epic independently. Full analysis, seam protocol, migration state inventory, and
determinism story in [docs/developer/decisions/spatial-sharding-design.md](decisions/spatial-sharding-design.md).

**2026-07-12 — Unified sensor vocabulary and the contact track model (Epic F, #677/#678).** Three
subsystems were each about to grow their own idea of "what can this thing see": player avionics
(#526), AI detection (#670), and missile seekers (#628/#676). The weapon TOML already carries a
`[seeker]` fragment (`fov_deg`/`acquisition_nm`) and ground/naval units a `[radar]` fragment
(`emitter_id`/`track_range_nm`/`can_shutdown`) — two vocabularies for the same physical question,
with a third implied by the AI. Locked before any of them ships, because the schema is
community-facing and fl-base-pack authors content against it. Jane's FA drove radar, IR, laser,
AWACS/GCI and AI visual acquisition from one schema; that is the design lesson taken here — a
clean-room reimplementation, not ported code.

- **One vocabulary, three consumers.** A single `SensorDef` schema — dual **search** and **track**
  lobes (azimuth/elevation half-angles, min/max range, per-check probability of detection) — serves
  avionics, AI, and seekers. A radar, an IRST, a laser designator and an eyeball differ in their
  *parameters*, not in their model.
- **`Contact` is the only downstream representation.** Everything below `SensorSystem` — AI
  conditions, Lua behaviors, avionics serialization (#526), datalink fusion (#528), missile guidance
  (#628) — reads **contact tracks with last-known state**, never ground-truth entity positions. This
  makes the "no omniscience" principle already normative in
  [docs/developer/ai-architecture.md](ai-architecture.md) *structurally* true rather than a rule to remember:
  the ground truth is not reachable from a consumer, so an omniscient AI cannot be written by
  accident. A stale or lost track is a first-class state, not a bug.
- **Signatures are unitless multipliers**, 1.0 = baseline fighter. Radar detection range scales by
  **`sqrt(sig)`**; IR and visual scale **linearly**. The square root echoes the fourth-root range
  dependence of the radar equation (RCS enters as the fourth power of range) closely enough to make
  stealth *feel* right and to keep authored numbers monotonic and intuitive, without dragging a real
  radar equation — and its calibration burden — into a content pack. Consistent with the fidelity
  pillar: the *behavior* is modelled, not the physics.
- **Probability of detection is defined per check at the reference 10 Hz cadence**
  (`[world] sensor_check_hz`, default 10). PoD is meaningless without a rate: the same 0.3 is a
  different sensor at 1 Hz than at 60 Hz. Authors tune against the reference cadence; an operator who
  changes it changes effective acquisition time, which is the honest consequence and is documented as
  such rather than silently renormalized.
- **The default sensor is an implicit builtin eyeball.** An AI-controlled entity that declares no
  `sensors` list gets a compiled-in visual-acquisition sensor (the `BuiltinFlightModel` pattern), so
  honest sensing is the default *everywhere* — including the zero-content sandbox. Sensing is not an
  opt-in feature flag: there is no configuration in which an AI sees through terrain, and no content
  pack can accidentally produce one by omission.
- **Library placement: a new `engine/sensor/` (`engine-sensor`, `fl::sensor`).** PUBLIC-links
  `engine-entity` + `engine-spatial`; `engine-net` PUBLIC-links it; `engine-ai`/`engine-script` link
  it PRIVATE. **`engine-entity` stays sensor-independent** (the signature POD lives in
  `engine/entity/`), so the entity system does not acquire a dependency on the thing that observes
  it. Enforced by `cmake/layering.cmake` like every other edge.
- **Emissions kernel: a per-observer `emitting` flag** (default true) that radar and laser **track**
  lobes require. This is deliberately a *seam*, not a feature — it is what SAM radar shutdown, EMCON
  discipline and RWR will later hang off (#526/#529). Landing the flag now costs one bool and keeps
  those behaviors from having to retrofit the sensor core.
- **Deferred migrations, named now so the fragments stop drifting.** The weapon `[seeker]` block
  (#583) and the ground/naval `[radar]` block (#526) in
  [docs/modding/formats.md](../modding/formats.md) become **references to sensor defs** rather than
  parallel schemas. They are not migrated in this record — the sensor core has to exist first — but
  their target shape is pinned here so new content is not authored deeper into a vocabulary that is
  already scheduled to be replaced.

Full decomposition: sensor core #677 (this record → #679 schema/parser → #680 entity fields → #684
detection math → #685 sensing pass → #686 tick-report schema), AI consumers under #670.

**2026-07-13 amendment (#684) — probability of detection gates ACQUISITION; geometry MAINTAINS the
contact.** A `pod` is the chance of *finding* something you were not already looking at. It is
**not** re-rolled to keep a contact you already hold: a target inside the cone stays held without a
die, and is lost when it leaves the cone or when its `lock_hold_s` coast expires — never because a
die came up short. Dice are therefore owed on exactly two edges, `Lost → Detected` and
`Detected → Locked`.

This is recorded as a decision rather than left in the implementation because it is the rule a
consumer is most likely to undo *by accident*: the obvious shape for a per-tick sensing loop is
"evaluate the sensor, roll, keep the contact if it passes", and that shape is wrong. A 0.35-PoD
radar re-rolled at the 10 Hz reference cadence drops and re-acquires an untouched target several
times a second — a flicker that is neither physical nor playable, and that no amount of downstream
smoothing can honestly repair. The rule binds the sensing pass (#685), player avionics (#526) and
missile seekers (#628/#676) alike; a consumer that wants a contact to decay must do it through the
coast, which is the mechanism that exists for exactly that purpose.

Two corollaries follow, and both are load-bearing:

- **Recovering a coasting contact costs no roll.** It was never lost, only unobserved.
- **A search-only sensor (`lock_hold_s = 0`, e.g. the builtin eyeball) drops the instant its target
  leaves the cone.** You have not "lost track" of something you were only ever looking at — you have
  simply stopped seeing it.

**2026-07-14 — Missile endgame resolution and the seeker vocabulary (Epic #583; resolves RFC
#676).** RFC #676 asked how a missile shot ends: full kinematic guidance-to-impact, a tabletop
probability-of-kill roll, or the hybrid it proposed — kinematic flyout with a data-driven terminal
Pk table. The RFC's motivations were cost at scale, netcode sensitivity, and countermeasures
degenerating to a binary hit/miss. All three predate the honest-sensing framework (#670/#677–#685),
which changes the answer:

- **Missiles fly kinematically to impact, and ALL probability lives in the seeker.** A seeker is
  the third consumer of the `SensorDef` vocabulary, evaluated through the same `Detection.h` math
  as every observer: PoD-gated acquisition and lock, geometry-maintained tracks, coast and
  geometric reacquisition — the 2026-07-13 "PoD gates acquisition" amendment binds seekers
  explicitly. Chaff and flares (#529) will modulate track-break and reacquisition through a seam on
  the seeker, which is where countermeasures stop being binary. The endgame itself is
  **deterministic geometry**: proximity-fuze radius → warhead application. There is no terminal Pk
  table — a missile that misses missed because of geometry the defender flew, not a die.
- Why the RFC's drawbacks no longer bind: the #573/#580 load characterization showed the
  integrate/AI phases stay cheap at 5000 entities (serialize is the cliff, and projectiles
  interest-filter like any entity); the quantized snapshot path (#515–#518) carries short-lived
  entities within budget; and the seeded-dice idiom (`detectionHash`) makes every roll reproducible
  from `(ids, tick)` — the replay/reconciliation requirement the RFC worried about is free.
  Probabilistic outcomes in PvP resolve to seeker dice a defender can influence (signature, aspect,
  countermeasures), not a fate roll at terminal range.
- **The weapon `[seeker]` migration named by the 2026-07-12 record is executed here**: `sensor_id`
  references a sensor def; the ad-hoc `fov_deg`/`acquisition_nm` lobe is deprecated (parsed for one
  release, warned on by the bootstrap and `validate-weapon`). What stays on the weapon is about the
  *shot*, not the sensor: `fire_and_forget`, `requires_designator`, `pitbull_nm` (ARH goes-active
  range — and the missile starts *emitting*, which is what RWR will see), and loft shaping
  (`loft_bias_deg`/`loft_range_nm`).
- **Deferred, named so they are not reinvented ad hoc:** a missile-launch/pitbull warning for the
  *targeted* player is RWR semantics and lands with #529 — wiring it off the cosmetic effect
  stream would be an omniscient warning (every launch in interest range, wallhack-grade). The
  weapon-audio layer built under #631 is #583's *mechanism* (one-shot 3D SFX with procedural
  fallbacks); the E-AUD epic (#586) owns everything continuous, looped or mix-managed and extends
  that mechanism rather than replacing it.

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
    getTrustLevel()       → TrustLevel { Unsigned | Community | Maintainer }
    isNativePlugin()      → bool
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

[mod.trust]          # optional — absent means Unsigned
signed-by = "community"   # "community" | "maintainer"; GPG verification is Phase 6
signature = "..."         # parsed and logged but not cryptographically verified until Phase 6
```

### Content Pack Layout on Disk

```
mods/
    example-content/          ← compiled content pack (ships as shared library)
        manifest.toml
        example-content.dll/.so
    fl-base-pack/             ← bundled default content (Phase 2+, fighters-legacy/fl-base-pack)
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

## Game Screen State Machine

The game binary (`fighters-legacy`) uses a `ScreenManager` that owns all menu and in-game screens, drives transitions between them, and fires side effects (mouse capture, server pause) on each transition.

### `Screen` enum

```
MainMenu → Loading → Flight → Pause → (Flight or MainMenu)
                 ↘ MissionSelect → MissionBrief → Loading
                 ↘ ReplaySelect → Loading (a replay session: no server, no socket)
MainMenu → Settings → MainMenu (or Pause)
Flight → Debrief → MainMenu
```

| Value | Description |
|---|---|
| `MainMenu` | Main menu; no local server running |
| `Loading` | Local server starting + ENet connecting; Quake-style progress messages |
| `MissionSelect` | Scrollable list of missions from content packs |
| `ReplaySelect` | Recorded matches (`.flrep`), newest first; an unreadable file is listed greyed out with the reader's refusal, never hidden (#41) |
| `MissionBrief` | Mission name, map placeholder, "Fly" / "Back" |
| `Settings` | Graphics and audio settings; saves on Back |
| `Flight` | In-flight; mouse captured; flight HUD + windshield rain rendered |
| `Pause` | Semi-transparent overlay; sim tick paused (server-side) |
| `Debrief` | Post-flight summary stub |
| `Quit` | Sentinel returned by screens to request application exit |

### `IScreen` interface

Every screen implements `IScreen`:
- `update(IInput&, IWindow&) → Screen` — called once per frame; returns the desired next screen (or the same screen to stay put)
- `buildElements() → span<HudElement>` — returns overlay elements to submit via `IRenderer::submitOverlayElements`

### `ScreenManager`

`ScreenManager` owns all `IScreen` instances as `unique_ptr`s. `LoadingScreen` and `FlightScreen` are re-created per session via `reinitLoading()` / `reinitFlight()` so callbacks capture fresh session objects.

`transition(Screen next)` fires two side effects in addition to updating the current screen:
- **Mouse capture**: `setMouseCapture(true)` entering Flight; `setMouseCapture(false)` entering any menu
- **Server pause** (single-player only; null in multiplayer): `serverCmd("pause")` entering Pause; `serverCmd("resume")` leaving Pause

### Session lifecycle

| Phase | Server | ENet client |
|---|---|---|
| Main menu | Not running | Not connected |
| "Fly" selected → Loading | Starts in background thread | Connecting to 127.0.0.1:4776 |
| Flight / Pause / Debrief | Running | Connected |
| "Quit to Menu" | Stopped | Disconnected |

`Game::startGame()` launches the server thread and creates session objects. `Game::stopGame()` joins the thread, disconnects ENet, resets `SimRenderBridge`, and clears session state. Between sessions the main menu is shown with no server overhead.

### Client-Side Prediction

`ClientPrediction` (`game/fighters-legacy/`) reduces perceived input lag by running a local
`FlightIntegrator` that mirrors the server's physics for the player's own entity only:

- **History ring**: a 128-slot plain-C-array ring of `{seqNum, BufferedInput}` entries. On
  each `MsgClientInput` sent, the input is pushed into the ring and the local integrator is
  stepped one tick (steady wind from `EnvironmentState`; turbulence is excluded — it is
  stochastic server-side and cannot be replicated without a future seed-broadcast).

- **Reconciliation**: `ClientNetEventHandler::snapshotCallback` fires after each snapshot is
  assembled, before `publishExternal()`. The integrator is reset to the server's authoritative
  `FlightState` (reconstructed from the player's `EntityRenderEntry`, including the new `omega`
  body-frame angular-rate field), and the last `estimatedDelayTicks` history inputs are
  replayed forward. The replay depth is carried losslessly in the `SnapshotPeerDelayTicks` TLV
  (0x0102). The player's entry is then overwritten with the predicted state in-place; all other
  entities remain server-authoritative with velocity extrapolation unchanged.

- **Correction**: divergence > `snap_threshold_m` (default 5 m) → hard snap; smaller
  divergence → position blend at `blend_rate` per reconciliation. Both values are in
  `[prediction]` user.toml.

- **Non-Earth gravity**: if `MsgConnectAck::planetRadiusKm` differs from 6371 km by more
  than 1 km, a custom `CentralGravityField` is constructed and injected into the local
  integrator so prediction physics matches the server.

## World Terrain Architecture

The engine uses a single continuous **world terrain** rather than per-theater heightmap grids. Players can fly anywhere in the world in a single session; theater boundaries are mission conditions (triggering failure when crossed), not engine limits.

### World coordinate system

All entity positions, terrain queries, and camera origins use **double-precision (`glm::dvec3`)** world coordinates throughout the engine — `EntityTransform::pos`, `MsgEntityEntry::pos`, `EntityRenderEntry::position`, and `CameraView::worldOrigin` are all `double`/`dvec3`. The coordinate space is right-handed Y-up, in meters, matching glTF. Camera-relative rendering subtracts `worldOrigin` before GPU upload and casts the small relative offset to `vec3` — float32-safe at any scale including planet-scale distances. The flight integrator's body-frame velocity (`FlightState::vel_body[3]`) is also `double`, enabling ICBM-range trajectory precision without accumulation error; `MsgEntityEntry::vel float[3]` and `EntityRenderEntry::velocity glm::vec3` remain `float` — dead-reckoning over a single frame (~16 ms) requires no more than float precision.

### Spherical-Earth world model

Spherical-Earth physics and terrain curvature is the engine's only supported mode. There is no flat-Earth fallback. The planet radius defaults to 6 371 000 m (Earth); non-Earth servers set `[world] planet_radius_m` in `server.toml`.

**Coordinate convention:** the world origin (`{0, 0, 0}`) is lat=0°, lon=0°, alt=0 m (mean sea level). The planet centre sits at `{0, -R, 0}` in world space, where R = `planet_radius_m`.

**Control-source seam (`IEntityController`):** `IEntityController::sample(EntityState, tick, dt, SpatialIndex* si = nullptr) → ControlInput` decouples the flight integrator from any assumption about who (or what) is flying. `WorldBroadcaster` maintains an `EntityId`-keyed registry of `ControlledEntity{sim, controller}` structs and steps every one uniformly each tick: `controller->sample()` produces inputs, `FlightIntegrator::step()` advances physics, and the result serialises into `MsgWorldSnapshot` automatically. Three concrete driver types all implement the same interface: `PeerController` (drains one input per tick from the peer's `JitterBuffer` — `engine/net/JitterBuffer.h` — and stale-repeats the last drained input when the buffer is empty, preventing coasting under packet loss; buffer depth is seeded from `estimatedDelayTicks` on first input and then adaptively resized each tick via an EWMA of delay and inter-arrival jitter (`depth = ceil(ewma_delay + k × jitter)`, capped by `[world].jitter_buffer_depth`)), C++ autopilot controllers in `engine-ai`, and `LuaController` in `engine-script` (a sandboxed Lua 5.5 script that exposes entity state, `guidance.*` math, and spatial queries). Register a server-side controller via `WorldBroadcaster::registerController(id, controller, model)` after spawning an entity.

**`engine/ai/` — server-side autopilot controllers (`engine-ai` library):** Twelve concrete `IEntityController` implementations ship with the engine:

| Controller | Behaviour |
|---|---|
| `LoiterController` | Orbits a fixed center point at configurable radius, altitude, and direction (`LoiterDir::Clockwise` / `CounterClockwise`). |
| `WaypointController` | Flies a sequence of 3D waypoints in order; advances on 3D capture radius; optional loop. |
| `PursuitController` | Pure-pursuit intercept: steers toward a target entity's current position each tick. |
| `EvadeController` | Horizontal escape: inverts the pursuit heading error to bank away from a threat entity. |
| `BreakTurnController` | Two-phase defensive ACM: Roll phase (bank toward the threat bearing for a configurable duration) followed by Pull phase (maximum-G elevator with afterburner, open-ended). |
| `LeadPursuitController` | Proportional navigation intercept: aims at a predicted intercept point (`target.pos + target.vel × TTC × navGain`); `navGain=0` degenerates to pure pursuit. |
| `LagPursuitController` | Lag pursuit intercept: aims behind the target (`target.pos − target.vel × TTC × lagFraction`); keeps the attacker inside the target's turn circle without overshooting; `lagFraction=0` = pure pursuit. |
| `ImmelmannController` | Half-loop + roll to reverse heading: Pull phase (full elevator + afterburner), Roll phase, Done. |
| `SplitSController` | Roll inverted + pull through to reverse heading: Roll phase (speedbrake to limit entry airspeed), Pull phase, Done. |
| `HighYoYoController` | Overshoot correction: banks away from target and climbs to bleed speed, then reacquires. Three phases: Climb, Reacquire, Done. |
| `LowYoYoController` | Dive-and-cut-corner to close range on a turning target. Three phases: Dive, Pull, Done. |
| `StateMachineController` | Composition framework: sequences child controllers with priority-ordered `Condition`-gated transitions and `minDwellSeconds` hysteresis. Built-in conditions: `ThreatWithinRange`, `ThreatBeyondRange`, `HpBelow`, `AnyEntityWithinRange`, `Always`, plus `And`/`Or`/`Not` combinators. |

`Guidance.h` (header-only) provides the shared math: `bodyForward`, `horizontalHeadingError`, `pitchErrorFromAlt`, `bankToTurnAileron`, `coordinatedRudder`, `elevatorFromPitchError`. `AiControllerFactory.h` (header-only) exposes `createController(behavior, args, entityManager*)` for string-based instantiation from admin commands. See `docs/user-guide/controls.md` (AI behaviors table) for the full `--ai` argument reference; `--ai lua <script_name>` loads a Lua controller from a content pack's `ai/` directory.

**Lua AI (`engine-script` library):** `LuaController` wraps `LuaSandbox` and implements `IEntityController`. The script defines `function compute_control(state, tick, dt) → table`; the engine calls it every tick and maps the returned table fields to `ControlInput`. Pre-registered globals: `guidance.*` (six wrappers over `Guidance.h` math), `nearby_entities(cx, cz, radius_m)` (SpatialIndex range query), `get_entity(idx)` (full entity state lookup). Script state persists between ticks via module-level variables. See `docs/modding/ai.md` for the full API reference.

**Spatial partition (`engine-spatial` library):** `SpatialIndex` — 2D uniform spatial hash (XZ-plane bucketing, double-precision, **runtime-configurable cell size**, default 10 km). Cell size is set via `WorldBroadcaster::setSpatialCellSize()` / `[world] spatial_cell_size_km` (`0` = auto = `clamp(drawDist/32, 500 m, 10 km)`); a cell much smaller than the draw distance explodes the `queryRadius` cell count, so the auto heuristic bounds a full-radius query to ~64² cells (#573). `clear()` recycles each cell's vector into a spare pool — no per-tick bucket reallocation in steady state. Rebuilt from all live entity positions once per sim tick at the start of `WorldBroadcaster::onTick()` — via `EntityManager::forEach`, which is **O(liveCount)** (dense live-index list), so the rebuild does not pay for dead pool slots under spawn/reap churn. `queryRadius(center, radius, fn)` visits all candidate entities in O(cells × local density). `WorldBroadcaster::spatialIndex() const noexcept` exposes the current index for interest management (#346, implemented) and AoE warhead consumers (#356); `IEntityController::sample()` receives it as the optional `si` parameter so AI controllers (#353) can query neighbors inline without an extra pass. Thread model: rebuilt on the sim thread at the start of `onTick`; thereafter read **concurrently** by the data-parallel AI pass (`queryRadius` is `const` and read-only — see `engine-job` below). **Note:** the index is 2D (XZ plane); 3D distance culling for extreme-altitude engagements is tracked in a follow-on issue.

**Data-parallel sim tick (`engine-job` library):** `JobSystem` — a persistent worker pool (pure stdlib, `namespace fl`) with a blocking `parallel_for(count, grain, fn)` that splits a range into dynamically-claimed chunks across the workers + the calling thread. `WorldBroadcaster::onTick` runs the per-entity work as two parallel passes over the live controlled entities — an **AI pass** (`controller->sample()`, read-only on a consistent pre-step world snapshot) and an **integrate pass** (`stepFlightSim`, each worker writing only its own entity's state) — dispatched through the injected `JobSystem`, or inline when none is injected (unit tests, single-player). The path is **serial-equivalent by construction** (no cross-entity writes in the parallel region; per-entity deterministic turbulence RNG; no parallel float reduction), so results are bit-identical across worker counts and validated race-free under ThreadSanitizer. Sized by `[world] sim_worker_threads` (0 = auto, 1 = serial). This is Epic A (#494); the design rationale (data-parallel vs spatial sharding) and the #512 follow-on are in [docs/developer/decisions/server-job-system-design.md](decisions/server-job-system-design.md).

**Graceful overrun governor (`engine/net/TickGovernor`, #514):** parallelism raises the sim ceiling but does not make it infinite — when the tick's measured wall-time still exceeds its fixed-step budget under load, the server must shed work rather than spiral. `WorldBroadcaster` owns one pure-AIMD `TickGovernor` (no glm/entity deps, unit-tested like `CongestionController`), steps it each `onTick` from the prior tick's `TickProfiler::lastTotalMs()`, and folds a single `loadFactor ∈ [floor, 1]` into three composing levers on top of the per-client congestion response: server-wide **snapshot send-rate decimation** (`max(perPeer, governor)`), **per-client byte-budget scaling**, and **AI-sample decimation** for non-player entities (`(tick+idx)%stride`, a pure function with per-entity input caching — serial-equivalent and TSan-clean). The control signal is an EWMA of per-tick wall-ms, not the laggy 60 s p99. The **integrate pass is never decimated** (fixed-`dt` stability), and auto-reducing the sim Hz is deliberately rejected (it would desync the 60 Hz-hardcoded prediction/jitter/congestion math) — the `GameLoop` catch-up cap (`[world] max_catchup_ticks`, exposed via `totalDroppedTicks()`) absorbs a fully integrate-bound server as bounded time dilation. A healthy/disabled/never-overrun server holds `loadFactor == 1` (no behaviour change, no wire change). Surfaced via `status`/`tickstats` + `--metrics-json` (`ServerTickReport` schema v2: `load_factor`/`dropped_ticks`); see [docs/developer/decisions/server-job-system-design.md](decisions/server-job-system-design.md).

**Server tick-budget instrumentation (`engine-perf`, header-only):** `TickProfiler` measures the per-phase wall-time of each `WorldBroadcaster::onTick()` — `maintenance` (rate-limit prune, idle timeout, admin drains, spatial-index rebuild, input drain, jitter resize), `integrate` (`stepFlightSim` per entity), `ai` (controller `sample()` per entity), `collision` (`EntityManager::onTick`), `serialize` (telemetry + per-peer snapshot assembly/send + weather + shutdown), and `total` — keeping a rolling ring of the last ~60 s and computing min/mean/p95/p99/max plus an actual ring-derived tick-Hz on demand. `WorldBroadcaster::getTickBudget()` returns a mutex-guarded snapshot (safe from any thread). `fl-server` surfaces it three ways: the `status` and `tickstats` admin commands, and an atomic `--metrics-json` / `[metrics] tick_json_path` file export serialised through `ServerTickReport` (the same shape `bot_swarm` embeds as its `server_tick` block). This is the measurement foundation Epic A's job system optimises against and Epic G's Prometheus exporter (#546) reuses. The percentile math (`fl::Stats`/`computeStats`) lives in `engine/perf/Stats.h`, shared with the load-test tools via `tools/common/NetStats.h` (#513).

**World systems foundation (`engine-world` library):** pure value types shared by the (deferred) world-systems work — no behaviour yet. `AlertLevel` (`Peacetime`/`Elevated`/`Conflict`/`WarState`) and `EscalationStage` (`Clean`→`Hostile`) enums; `FactionDef` (id, name, starting alert level) and `FactionRegistry` — an O(1)-by-`uint16_t`-index faction store (`indexOf`/`get`/`count`, a symmetric `relationship` graph defaulting to `Neutral` off-diagonal / `Friendly` on the diagonal, and per-faction `alertLevel`); and the `AirspaceZone` descriptor (circle or convex-polygon XZ region with an altitude band, owner faction, and policy id). `EntityState` carries a `uint16_t factionIndex` (0 = neutral) that indexes the registry. `FactionRegistry` threading is three-tier: `m_defs`/`m_index` are immutable after `load()` (lock-free reads); the relationship matrix is sim-thread-only; only the per-faction alert levels are mutex-guarded, because `setAlertLevel()` may be called from the network/main thread. The consuming logic — `AlertSystem` (#162) and `IWorldAiProvider` (#163) — is deferred; this issue (#415) lands only the foundation so those can build without churn.

**Algorithm choice — uniform grid vs. tree-based index:** a tree-based index (quad-tree, k-d tree) adapts to non-uniform entity distributions but has O(N log N) rebuild cost and wins on range queries only when N > ~10,000 entities in highly clustered configurations. A combat flight sim distributes entities uniformly across large areas by the nature of flight dynamics; the "hot dogfight" case where many aircraft cluster in one 10 km cell simultaneously requires serializing all of them to nearby peers anyway, so no query work is saved by a better algorithm. The uniform grid handles Phase 4 targets (~628 entities) and Phase 8 stretch goals (~5,000 AI drones) comfortably.

**No `ISpatialIndex` interface:** `queryRadius` is a function template (`Fn&&`). Virtualizing it requires erasing `Fn` to `std::function`, adding allocator overhead and an indirect call on every per-entity callback at 60 Hz — pure cost at this entity scale. Algorithm is an implementation detail behind a stable concrete API (`clear`, `insert`, `queryRadius`); swapping to a different algorithm later means replacing `SpatialIndex.cpp` only, with no consumer changes — the same flexibility an interface would provide, at zero runtime cost.

**Gravity (`IGravityField` seam):** `CentralGravityField` (`engine/flight/CentralGravityField.h`) implements 1/r² falloff toward the planet centre; `earthInstance()` provides an Earth singleton (R = 6 371 000 m). The default for every `FlightIntegrator` and `WorldBroadcaster`. Swap via `FlightIntegrator::setGravityField()` or `WorldBroadcaster::setGravityField(field, planetRadiusKm)` for non-Earth planets. `WorldBroadcaster` records the radius for transmission to clients in `MsgConnectAck.planetRadiusKm`.

**Atmosphere altitude (`IGravityField::geodeticAltitude()`):** `FlightIntegrator` uses `m_gravity->geodeticAltitude(pos)` instead of `pos[1]` for ISA pressure-altitude lookup, so the correct atmosphere density is applied even when the entity is far from the Z-axis (where world-Y diverges from geodetic altitude).

**Geodetic utilities (`engine/flight/Geodetic.h`):** header-only inline functions — `worldToGeodetic(x,y,z)→LatLonAlt`, `geodeticToWorld(lla,x,y,z)`, `geodeticAltitude(x,y,z)` — convert between world XYZ and spherical lat/lon/alt. All angles in radians; `kEarthRadiusM = 6 371 000.0`.

**Terrain curvature (`TerrainStreamer::setPlanetRadius(double)`):** per-vertex spherical Y correction `sqrt(R²−vx²−vz²)−R` is baked into each vertex by `buildTerrainMeshGlb()` (via the `chunkWorldX`, `chunkWorldZ`, and `planetRadius` parameters); surface normals account for the curvature gradient too. `heightAt()` applies the same correction independently from the raw heightmap (thread-safe via `shared_mutex`). `getRenderItems()` places each chunk at Y = 0 in world space — all curvature is encoded in vertex data, so tiles visibly curve rather than lying flat. Default radius is 6 371 000 m (Earth). Must call `setPlanetRadius()` before the first `update()`; changing it after chunks are loaded leaves stale meshes.

**Client wiring:** `ClientNetEventHandler` reads `planetRadiusKm` from `MsgConnectAck` and exposes it via `planetRadiusKm()`. `Game` wires it to `terrainStreamer.setPlanetRadius()` on `Screen::Flight` entry.

### Chunk format

| Property | Value |
|---|---|
| Chunk size | 15,360 m (512 intervals × 30 m) |
| Resolution | 513×513 pixels, 16-bit grayscale PNG |
| DEM source | Copernicus GLO-30 (ESA, 30 m global, free) — no upsampling required |
| LOD 0 | 513×513 px, 30 m/px, streamed within ~46 km (3×3 chunks) |
| LOD 1 | 257×257 px, 60 m/px, streamed within ~77 km (5×5 chunks) |
| LOD 2 | 129×129 px, 120 m/px, streamed within ~107 km (7×7 chunks) |
| Eviction | Beyond ~120 km; max ~83 chunks in memory at steady state |

### Terrain ID and overrides

`"world"` is the canonical terrain ID. fl-base-pack provides global coverage at base priority. Theater content packs override individual tiles at higher mod priority via `IContentPack::resolveTilePath(terrainId, face, level, i, j, layer)` (#473) — the engine walks the content stack and uses the first pack that resolves a given cube-sphere tile.

### Theaters

Theaters are geographic bounding boxes in world coordinates defined by `theaters/<id>.toml` in a content pack. They are mission-layer concepts: the engine does not partition terrain by theater, and terrain streaming is always camera-driven across the full world grid.

---

## Repository Naming Convention

All first-party repositories and binaries in the fighters-legacy ecosystem use the `fl-` prefix. Bridge plugins for specific external games use a `<game>-bridge` pattern (they provide no content of their own; they bridge the user's own install of that game). Core repositories keep their full names.

| Pattern | Examples |
|---|---|
| `fl-<name>` | `fl-server` (dedicated server), `fl-lobby` (matchmaking service), `fl-base-pack` (bundled default content) |
| `<game>-bridge` | `fa-bridge` (Jane's Fighters Anthology bridge plugin, renamed from `fa-content`) |
| Full name | `fighters-legacy` (engine + game), `fighters-codex` (reference repo) |

---

## Key Design Constraints

- **Cross-platform from day one.** All code compiles on MSVC (Windows), GCC/Clang (Linux), and AppleClang (macOS). Platform-specific paths are confined to `platform/`.
- **`IContentPack` is the only content boundary.** Adding a new content source means implementing this interface, not modifying the engine.
- **C++20, no extensions.** `CMAKE_CXX_EXTENSIONS OFF` is enforced across all presets.
