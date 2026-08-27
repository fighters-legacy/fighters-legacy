# Development Guide

## Optional: voice wingman commands (`FL_ENABLE_WHISPER`)

The deterministic voice-command tier (#935) — hold a key, say "two, engage bandits", release — needs
the whisper.cpp speech-to-text backend, which is **off by default**:

```bash
cmake --preset debug -DFL_ENABLE_WHISPER=ON
```

Off, `platform-stt` still builds and hands back `NullSpeechToText`, so nothing else in the tree
changes shape and the in-game radio menu is the way to order your flight (decision #769).

**No model ships with the game.** Model size is a player's trade of accuracy against RAM and CPU, and
bundling one would add hundreds of megabytes to every install for a feature most players will not
use. Point `[voice] stt_model_path` in `user.toml` at a downloaded `ggml-*.bin`; `base.en` is a
reasonable starting point for six short commands.

The build is **CPU-only on purpose**. Every ggml accelerator (CUDA, HIP, Metal, Vulkan, SYCL, OpenCL,
BLAS, OpenMP) is explicitly disabled in `cmake/dependencies.cmake`. This tier exists precisely
because it does not need a GPU, and a build that quietly picked up CUDA because the machine happened
to have the SDK installed would not be the thing that was measured. See the comment there — it is the
cpp-httplib lesson from #233, where `REQUIRE_*` turned out not to be the off switch.

**Verification status.** `FL_ENABLE_WHISPER=ON` was configured and compiled on Linux/GCC 16 during
the v0.4.0 documentation audit (#1047): the lockdown reports `whisper.cpp: enabled (CPU-only; every
ggml accelerator explicitly disabled)` at configure time, and `libwhisper.a` and `libplatform-stt.a`
both link. It has **not** been built on Windows or macOS, and **no CI leg exercises it** — the
shipped releases are all `OFF`. Treat the flag as working-but-unguarded: a change to the dependency
block will not be caught by anything until someone builds it again by hand.

## Prerequisites

### Linux — Fedora (primary maintainer platform)

```bash
sudo dnf install cmake ninja-build gcc g++ clang clang-tools-extra \
  vulkan-devel SDL3-devel openal-soft-devel lcov \
  libasan libubsan glslang vulkan-validation-layers lua-devel \
  openssl-devel protobuf-devel protobuf-compiler
```

`libasan` and `libubsan` are required for the `asan` build preset (`-fsanitize=address,undefined`). They are separate packages from `gcc` on Fedora.

`glslang` provides `glslangValidator`, the GLSL-to-SPIR-V compiler used to build the Vulkan renderer shaders at CMake configure time.

`vulkan-validation-layers` provides `VK_LAYER_KHRONOS_validation`, enabled automatically in debug builds via `FL_VK_VALIDATION`. Without it the renderer still works but validation errors go unreported.

For Bluetooth gamepad support (Xbox controllers), see [docs/user-guide/gamepad-linux.md](../user-guide/gamepad-linux.md).

> **Note:** `SDL3-devel` and `openal-soft-devel` are optional. CMake automatically fetches and statically compiles both when they are absent — this is what CI and release builds do. Installing them speeds up local dev builds but means your dev binary will dynamically link those libraries, unlike the self-contained CI and release artifacts.

> **Note (OpenAL):** Some Fedora installs ship `/etc/openal/alsoft.conf` with `drivers = null`, which silently discards all audio. If `audio_check` reports success but you hear nothing, override it: `printf '[general]\ndrivers = pipewire\n' > ~/.config/alsoft.conf`

### Linux — Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build gcc g++ clang clang-format \
  libvulkan-dev libsdl3-dev libopenal-dev lcov glslang-tools \
  vulkan-validationlayers libssl-dev libprotobuf-dev protobuf-compiler
```

`glslang-tools` provides `glslangValidator` for Vulkan shader compilation. `vulkan-validationlayers` provides `VK_LAYER_KHRONOS_validation` for debug builds.

> **Note:** `libsdl3-dev` and `libopenal-dev` are optional. CMake automatically fetches and statically compiles both when they are absent — this is what CI and release builds do. Installing them speeds up local dev builds but means your dev binary will dynamically link those libraries, unlike the self-contained CI and release artifacts.
>
> **Note (Lua):** `liblua5.5-dev` is not yet available in Ubuntu apt. Lua 5.5 is always built from source via FetchContent on Linux — no extra install needed.
>
> **Note (GameNetworkingSockets):** `libssl-dev` + `libprotobuf-dev` + `protobuf-compiler` are the deps for the GNS transport backend (`FL_ENABLE_GNS=ON`, the default). GNS itself is built from source (FetchContent, static). protobuf is **system-preferred** (find_package); OpenSSL is the crypto backend. Build `-DFL_ENABLE_GNS=OFF` for a lean enet6-only build without these. See [gns-backend.md](gns-backend.md).

### Windows (MSVC 2026)

1. Install [Visual Studio 2026](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload
2. Install the [Vulkan SDK](https://vulkan.lunarg.com/) (1.3 or later) — includes `glslangValidator`, `VK_LAYER_KHRONOS_validation`, and MoltenVK support headers
3. **Ninja** — required by the `debug-msvc` and `release-msvc` presets. Two options:
   - Install system-wide (works from any PowerShell terminal): `winget install Ninja-build.Ninja`
   - Use the copy bundled with VS: open **Developer PowerShell for VS 2026** or **x64 Native Tools Command Prompt for VS 2026** — both put Ninja on `PATH` automatically. VS Code with the CMake Tools extension also finds the VS-bundled Ninja without any extra steps.
4. **clang-format-22**: CI pins clang-format-22 (LLVM 22). Install via:
   ```powershell
   winget install LLVM.LLVM
   ```
   or download the installer from [releases.llvm.org](https://releases.llvm.org/). Add the LLVM `bin/` directory to `PATH`.
5. **Pre-commit hook**: `scripts/hooks/pre-commit` is a bash script. On Windows it must be run via **Git Bash** or **WSL** — it will not work in PowerShell or cmd. The DCO commit-msg hook has the same requirement.

> **Build performance tip:** Windows Defender scans every `.obj` and `.lib` as it is written, which adds significant time to cold builds. Exclude the build output directory to avoid this:
> ```powershell
> Add-MpPreference -ExclusionPath "$PWD\build"
> ```
> Run once from the repo root in an elevated PowerShell. The exclusion persists across reboots.

> **Note (Lua):** Lua 5.5 is not required on Windows — CMake automatically fetches and compiles it via FetchContent.

> **Developing on Linux or macOS?** [`tools/windows-env/`](https://github.com/fighters-legacy/fighters-legacy/blob/main/tools/windows-env/README.md) is an optional Vagrant VM that runs the MSVC build, `ctest` and a rendered screenshot smoke against your branch, so an MSVC-only failure (a shadowed local, a narrowing conversion, a missing `<string>` include) surfaces before the push. It is not a gate and no hook invokes it.

### macOS (Apple Silicon, 13+)

```bash
xcode-select --install
brew install cmake ninja vulkan-headers molten-vk vulkan-loader glslang
```

`molten-vk` provides the Vulkan-over-Metal ICD; `vulkan-loader` provides `libvulkan.dylib`; `glslang` provides `glslangValidator`. Validation layers are not available via Homebrew — install the [LunarG Vulkan SDK for macOS](https://vulkan.lunarg.com/) to get `VK_LAYER_KHRONOS_validation`.

> **Note:** CI uses the Homebrew path (no validation layers). For local dev, the LunarG SDK is recommended so validation errors are caught before CI.

> **Note (compiled content pack plugins):** Unsigned `.dylib` compiled plugins require the user to allow them via **System Settings → Privacy & Security** the first time they are loaded. This is a macOS Gatekeeper requirement and cannot be bypassed by the engine.

### Optional tools

**REUSE** — checks SPDX license compliance. CI enforces this automatically via `fsfe/reuse-action`; install locally for fast feedback before pushing:

```bash
pip install reuse
```

Run with `reuse lint` from the repo root.

**gcovr** — required to run coverage gates locally (the `coverage` preset instruments the build; gcovr reads the `.gcda` files and enforces thresholds). CI installs it automatically; install locally to run gates before pushing:

```bash
pip install gcovr
```

See [Testing → Code coverage](#code-coverage) for the full local workflow.

**gh (GitHub CLI)** — used by `scripts/roadmap-status.sh` and `scripts/prune_merged_branches.py`. Both scripts degrade gracefully without it, but `prune_merged_branches.py` will miss squash-merged and rebase-merged branches if `gh` is not authenticated. Install from [cli.github.com](https://cli.github.com) and authenticate with `gh auth login`.

<!-- REUSE-IgnoreStart -->
Copyright is declared centrally in `REUSE.toml` rather than in each file. All `.h` and `.cpp` files are covered by a glob annotation there — new source files do not need an in-file `SPDX-FileCopyrightText` line. The `// SPDX-License-Identifier: GPL-3.0-or-later` line in each source file is still required.
<!-- REUSE-IgnoreEnd -->

### Third-party dependencies & licenses

Dependencies are pulled via FetchContent or system packages (they are **not** vendored in-tree, so REUSE does not scan them), but a distributed **binary** must reproduce their notices. All are GPL-3-compatible:

| Dependency | License | Notes |
|---|---|---|
| GameNetworkingSockets | BSD-3-Clause | GNS transport (`FL_ENABLE_GNS`) |
| Protobuf | BSD-3-Clause | GNS dependency (system-preferred) |
| OpenSSL | Apache-2.0 | GNS crypto backend |
| enet6 | MIT | enet6 transport |
| libogg / libvorbis | BSD-3-Clause | OGG Vorbis audio decode (engine-audio; FetchContent-pinned, #723) |
| Opus | BSD-3-Clause | Voice codec for the in-game radio nets (engine-voice; system-preferred, FetchContent fallback, #531) |
| Dear ImGui | MIT | IGui HAL backend (`platform-gui`; FetchContent-pinned, Vulkan-gated, #156) |
| SDL3 / OpenAL Soft / GLM / KTX / Lua / tomlplusplus / Catch2 | zlib / LGPL-2.1 / MIT / Apache-2.0 / MIT / MIT / BSL-1.0 | see `cmake/dependencies.cmake` |

> Release packaging (`release.yml`) is responsible for bundling the BSD-3-Clause / Apache-2.0 / MIT notice texts alongside the shipped binaries (the GPL source-offer covers the rest). Tracked as a release-hardening follow-up.

### Go toolchain (cluster + live-services repos)

The C++ engine, game, and `fl-server` need only the CMake toolchain above. The 128+
multiplayer re-target adds **Go** companion repositories that build independently of the CMake
matrix:

- `fl-operator` — Kubernetes / OpenShift operator built with the [Operator SDK](https://sdk.operatorframework.io/) / Kubebuilder (Agones-native).
- `fl-account` — pluggable identity / account service.
- `fl-review` — offline anti-cheat batch-analysis service.

To work on those repos you need a recent **Go toolchain** (see each repo's `go.mod` for the
pinned version), plus — for the operator — a local Kubernetes (kind/minikube) and the Agones
chart for end-to-end testing. These tools are **not** required to build or run the game or
`fl-server`; they live in separate repositories with their own Go CI lanes. New Go files carry
<!-- REUSE-IgnoreStart -->
`// SPDX-License-Identifier: GPL-3.0-or-later` headers per REUSE, same as the C++ tree.
<!-- REUSE-IgnoreEnd -->

---

## Building

All platforms use CMake presets — no raw flag strings needed.

```bash
# Linux / macOS
cmake --preset debug
cmake --build --preset debug

# Windows (PowerShell)
cmake --preset debug-msvc
cmake --build --preset debug-msvc
```

### Running tests

```bash
ctest --preset debug --output-on-failure          # Linux / macOS
ctest --preset debug-msvc --output-on-failure     # Windows
```

### Available presets

| Preset | Platform | Use |
|---|---|---|
| `debug` | Linux / macOS | Development (Werror ON) |
| `release` | Linux / macOS | Packaging |
| `release-headless` | Linux / macOS | Headless `fl-server` + `bot_swarm` (no Vulkan, no GNS) — the scale gate |
| `release-headless-gns` | Linux / macOS | Same, with GameNetworkingSockets — the reference runner |
| `debug-msvc` | Windows | Development (Werror ON) |
| `release-msvc` | Windows | Packaging |
| `coverage` | Linux / macOS | Coverage reporting (Werror OFF) |
| `asan` | Linux / macOS | AddressSanitizer + UBSan |
| `tsan` | Linux / macOS | ThreadSanitizer (data races) |

CI uses `debug` (Linux/macOS) and `debug-msvc` (Windows). The `coverage`, `asan`, and `tsan` presets have their own dedicated CI jobs, and the scale gate uses `release-headless` / `release-headless-gns`.

⚠ **The two headless presets exist so that turning Vulkan off gets its own `binaryDir`** (#1354). Passing `-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON` to `--preset release` instead caches it into the shared `build/release` tree, and since the game client is guarded on `if(TARGET platform-sdl3 AND TARGET platform-vulkan)`, that tree then has **no `fighters-legacy` target** — while `cmake --build --preset release` keeps exiting 0 and the pre-reconfigure client binary keeps sitting there being run. A configure that skips the client now says so with a `message(WARNING)`.

### Sanitizers: ASan and TSan catch different bugs

They are **not** two settings of the same dial, and neither subsumes the other. A data race is invisible to ASan; a use-after-free is invisible to TSan.

| | **`asan`** (AddressSanitizer + UBSan) | **`tsan`** (ThreadSanitizer) |
|---|---|---|
| Finds | Memory errors — heap/stack/global buffer overflows, use-after-free, use-after-scope, double-free, leaks. Plus UBSan: signed overflow, null/misaligned deref, bad shifts, invalid enum/bool values | **Data races** — two threads touching the same memory unsynchronized, at least one writing. Also lock-order inversions |
| Scope | The **whole** test suite: any line of code can have a memory bug | Only the genuinely concurrent code: `test_job_system` + `test_world_broadcaster` (the data-parallel sim tick). The rest of the engine is single-threaded, so TSan there finds nothing and costs a lot |
| Cost | ~2× slower, ~3× memory | ~5–15× slower, ~5–10× memory |

**They are mutually exclusive** — the two instrumentations conflict and cannot be built together. That is why they are two separate CI jobs (`asan.yml`, `tsan.yml`) rather than one, and why a bug that reproduces under one may need the other to explain it.

⚠ **Do not run the full suite under `tsan`.** `ctest --preset tsan` fails ~26 network tests, and has for a long time: TSan's instrumentation slows the ENet/GNS handshake past the fixed pump budgets those integration tests allow, so they time out. This is a property of the sanitizer, not a bug in the transport. `tsan.yml` therefore runs the two scoped binaries **directly**, not through ctest:

```bash
CC=clang CXX=clang++ cmake --preset tsan
cmake --build --preset tsan --target test_job_system test_world_broadcaster
TSAN_OPTIONS=suppressions=$PWD/tools/tsan.supp ./build/tsan/tests/test_job_system
```

ThreadSanitizer is **Linux/macOS only** (no MSVC support); on Fedora it needs the `libtsan` package, on Debian/Ubuntu it ships with `clang`/`gcc`. `tools/tsan.supp` suppresses races inside third-party threads.

### Tests run in parallel

The `debug`, `release`, `debug-msvc` and `release-msvc` test presets set `execution.jobs = 8` (`asan` uses 4, for sanitizer memory), so `ctest --preset debug` is parallel by default — **79 s → 18 s** on a 24-core box. Override for a bigger machine with `ctest --preset debug -j 24`.

⚠ **`jobs` must be an explicit bound. Do not set it to `0`.** CTest does *not* read 0 as "one per core" — it treats it as *unlimited* and oversubscribes (measured: 53 concurrent processes on a 24-core box). That is survivable on a big Linux box and fatal on a macOS runner, where it exhausts the process limit and ~1600 tests never start — reported as `***Not Run`, not as failures. A bound of 8 is no slower in practice anyway: wall-clock is set by the slowest few tests, not by throughput.

Two presets stay **serial**, deliberately:

- **`coverage`** — concurrent test processes writing the same `.gcda` counter files is a known way to corrupt coverage data, and the coverage leg is advisory rather than a required check, so its wall-clock does not hold up a merge.
- **`tsan`** — see the warning above; it is not run through ctest at all.

Because each `TEST_CASE` is registered as its own ctest test, parallel runs execute them as **concurrent processes**. A test may therefore not assume it is the only process on the machine (#787):

- **Never hardcode a port.** Bind an ephemeral port (`bind(addr, 0, n)`) and read it back with `ENetNetwork::boundPort()`, or ask the OS for a free one. Two tests once shared port 19009 and only "worked" because they never ran at once.
- **Never name a fixed temp path**, and note that a *per-process counter is not unique* — it restarts at 1 in every process. Use `fl::test::uniqueTempPath()` / `TempDirGuard` from `tests/temp_path.h`, which salt the name with a per-process token.
- **Do not sleep a fixed interval and hope** a packet/file arrived. Poll to a deadline; a sleep that is too short on a loaded machine passes for the wrong reason.

### Local preset overrides

Create a `CMakeUserPresets.json` in the repo root to override preset defaults locally (e.g. a different binary directory or a custom toolchain path). This file is gitignored.

---

## Testing

For running the test suite see [Building → Running tests](#running-tests).

### Test names are ASCII

`catch_discover_tests()` registers one ctest test per Catch2 case using the case's **name**, and
ctest runs it by passing that name straight back to the binary as a filter argument. On Windows that
round trip is not UTF-8, so a non-ASCII character arrives mangled, the filter matches nothing, and
ctest reports a **failure** for a test that never ran:

```
Filters: "AtcFacility: a landed flight is never retired ? pinned pending #1149"
No test cases matched
***Failed
```

Linux and macOS run it green, so the diagnosis costs a full CI cycle and the failure text points at
the wrong subsystem. Keep `TEST_CASE` names and tags to ASCII — use `--` or parentheses instead of an
em dash, straight quotes instead of curly ones. `tools/lint_test_names.py` enforces this in the
`lint` job, before the build matrix spends macOS and Windows minutes.

**This applies only to names and tags.** Comments, log strings, assertion text and localisation data
are UTF-8 throughout and should stay that way; the linter reads nothing but the registration macros'
string literals.

### Code coverage

The `Coverage` workflow runs on every pull request and on every push to `main`, and applies these
gates:

| Scope | Metric | Threshold |
|---|---|---|
| `engine/` | Branch coverage | ≥ 80% (CI enforced) |
| `engine/` | Line coverage | ≥ 70% (CI enforced) |

It is **advisory** — it is not a required check, so a red coverage run does not block a merge; it
tells a reviewer that the change lowered coverage. On `main` its verdict is carried by an
automatically opened tracking issue that closes itself on the next green run, because a workflow
that only reports through a red X in the Actions list can stay broken unnoticed. It did, for three
weeks (#1128).

`engine/` branch coverage is **80.3%** and the gate passes (#1145, 2026-08-07). It had been 75.09%
— the first real measurement since 2026-06-29, taken after the engine grew from 21.7k to 74.3k lines
while the gate could not measure anything, so the new code had arrived below the bar. The threshold
was held at 80 and the shortfall paid down rather than absorbed by moving the number.

The margin above the bar is thin by design but not razor-thin: branch counts differ slightly between
compilers, so a change that lands the local measurement at exactly 80.0% can still fail on CI. Aim
for headroom rather than the bar itself.

`platform/` backends, `tools/`, and `game/` are excluded from coverage reporting. Branch
coverage catches untested conditional paths and is the primary gate; line coverage is a
secondary floor. `platform/` is excluded because platform divergence makes unit testing
brittle. `game/` is excluded in Phase 1 — the binary is a stub with no game loop; its
coverage gate will be added when Phase 2 game logic lands.

Gates use `--exclude-throw-branches` (gcovr 8.x): GCC instruments every non-`noexcept`
call site with an "exception throw" branch that is never taken in normal unit tests.
Excluding these focuses the gate on meaningful decision branches (if/else, switch).

**A coverage failure always says which kind it is.** `tools/coverage_gate.py` runs `gcovr` once and
distinguishes three outcomes, because conflating the last two is what hid a three-week outage
(#1128) behind a step named after the threshold:

| Exit | Meaning | What the message contains |
|---|---|---|
| 0 | Pass | The measured percentages and the thresholds they cleared |
| 1 | Below threshold | The measured percentage, the limit, and the lowest-covered files |
| 2 | **Not measured** | Why no number exists — and deliberately no percentage at all |

"Not measured" covers a `gcovr` parse abort, a missing or unparseable summary, filters that matched
nothing (`gcovr` reports that as 0.0% and exits 0), and a zero denominator. Each of those can
produce a plausible-looking number that means nothing, so none of them is allowed to reach a
threshold verdict.

An HTML coverage report is uploaded as a CI artifact on every run (retained 30 days), alongside the
machine-readable `coverage-summary.json` the gate ran against. Access them from **Actions** → select
a run → **Artifacts**. Codecov is uploaded *before* the gate runs, so the numbers are available for
exactly the run that failed.

**Running coverage locally (Linux/macOS — requires GCC or Clang):**

```bash
cmake --preset coverage
cmake --build --preset coverage
ctest --preset coverage --output-on-failure

# Capture and filter (engine/ only, with branch data; game/ excluded until Phase 2)
lcov --capture --directory . --output-file coverage.raw \
     --branch-coverage --ignore-errors empty,source,gcov,negative
lcov --remove coverage.raw '/usr/*' '*/tests/*' '*/vendor/*' '*/_deps/*' \
     '*/platform/*' '*/tools/*' '*/game/*' \
     --output-file coverage.info \
     --branch-coverage --ignore-errors empty,unused,source,gcov

# Check the gates exactly as CI does (requires: pip install gcovr)
python3 tools/coverage_gate.py --filter 'engine/' --gcov-exclude '.*/_deps/.*' \
    --exclude-throw-branches --min-branch 80 --min-line 70

# Generate HTML report
genhtml coverage.info --output-directory coverage-report --branch-coverage
# Open coverage-report/index.html
```

Codecov posts a coverage delta comment on each pull request, comparing against `main`'s most recent
run.

---

## Fuzzing

Binary-format parsers and network packet handlers are the engine's highest-risk attack
surfaces — a malformed content pack or a spoofed packet could trigger memory corruption.
[libFuzzer](https://llvm.org/docs/LibFuzzer.html) harnesses under `fuzz/` exercise these
parsers on adversarial input under AddressSanitizer + UndefinedBehaviorSanitizer. The
`fuzz` preset is **clang-only and Linux-only**; the MSVC/GCC build matrix is unaffected.

**Build and run all harnesses:**

```bash
cmake --preset fuzz
cmake --build --preset fuzz --target fuzzers
ctest --preset fuzz --output-on-failure   # 60 s smoke per harness over its seed corpus
```

Each `fuzz_smoke_<name>` ctest replays the committed seeds in `fuzz/corpus/<name>/` plus a
build-tree working corpus for 60 s. Requires clang with the libFuzzer runtime
(`libclang_rt.fuzzer`, bundled with the `clang` package on Ubuntu).

Harnesses cover the network codecs, the content/asset parsers, and the on-disk formats — including
`fuzz_flrep`, the replay reader, which parses a file a player downloads from a stranger
(docs/developer/replay-format.md §5).

**Run one harness manually (longer, to explore new states):**

```bash
./build/fuzz/fuzz/fuzz_snapshot_codec fuzz/corpus/fuzz_snapshot_codec -max_total_time=300
```

### Coverage

- **Network codecs** (pure): `fuzz_snapshot_codec`, `fuzz_wire_tlv`, `fuzz_rcon_packet`.
- **Message handlers** (stateful, over the `tests/` mocks): `fuzz_server_msg`
  (`WorldBroadcaster::onReceive`), `fuzz_client_msg` (`ClientNetEventHandler::onReceive` — the widest
  untrusted client surface). Both use `fuzz/FuzzFrames.h` to pack several `onReceive` packets into one
  input so a single corpus entry reaches the multi-packet states (chunk reassembly, delta-after-full
  snapshot decode, ack advance).
- **The connect path** (`fuzz_connect_path`, #1073): the only harness that COMPLETES a handshake.
  `fuzz_server_msg` calls `onConnect` and stops, which leaves two areas unreachable from it — the
  `handleConnectRequest` parser (a `packCount × PackManifestEntry` record loop plus three TLV parses
  at an attacker-controlled offset) and every handler gated on `handshakeComplete` (chat, voice, seat,
  team). This harness drives the first fuzz frame into the connect path and feeds the rest to a
  separately-admitted peer, with the radio-net table populated so the voice relay fan-out actually
  runs. Measured against `fuzz_server_msg` on equal-length runs: `handleChat`, `handleVoiceFrame` and
  `selectVoiceRecipients` go from **zero** hits to covered.
- **Content / asset parsers**: `fuzz_asset_validator`, `fuzz_terrain_png`, `fuzz_ogg` (the
  libvorbis-backed `decodeOgg` + streaming API — the untrusted content-pack audio path, #723),
  `fuzz_flight_model_toml`, `fuzz_entity_def_toml`, `fuzz_playlist_toml`, `fuzz_server_config_toml`,
  `fuzz_mod_manifest`, `fuzz_mesh_json`.

### Vendored-parser sanitizer configuration

Fuzzing drives vendored C/C++ parsers (toml++, stb_image, tinygltf) on adversarial input, where they
trip a few **standard-UB-but-benign** idioms that only fire under a debug + sanitizer build. The fuzz
build neutralizes exactly these, and nothing more (see the `FL_BUILD_FUZZERS` block in the root
`CMakeLists.txt`):

- `TOML_ASSERT` is `#define`d to a no-op — toml++'s internal debug assert would otherwise `abort()`
  on malformed TOML the parser is meant to reject via `toml::parse_error` (release defines `NDEBUG`).
- `-fno-sanitize=unreachable,pointer-overflow,nonnull-attribute` — toml++/stb use
  `__builtin_unreachable` for "can't happen for valid input" branches, the `if (buf + n <= end)`
  reader idiom (forms the past-the-end pointer *before* the bounds check), and zero-length
  `memcpy(nullptr, _, 0)`. None is an actual memory error.

Engine code relies on none of these idioms, so **every other UBSan check plus all of ASan's
memory-safety checks (OOB read/write, use-after-free, real null deref) stay on** — a genuine
fighters-legacy bug is still caught; only the vendored parsers' pedantry is quieted.

### Disabled harnesses

A harness that targets a real surface but depends on a vendored decoder with **actual** memory-safety
defects on malformed input (memory corruption, not benign UB) is parked under `fuzz/disabled/` — not
built, not run, out of the smoke enumeration — with the reproducers and a re-enable checklist in
`fuzz/disabled/README.md`. **None currently parked.** (`fuzz_ogg` lived there while OGG decode was
backed by stb_vorbis, a trusted-input decoder that SEGVs on malformed streams; #723 replaced it with
the reference libvorbis decoder and the harness moved back into the smoke.)

### Adding a harness

1. Write `fuzz/fuzz_<name>.cpp` with an `extern "C" int LLVMFuzzerTestOneInput(const
   uint8_t* data, size_t size)` that drives one parser (fail-soft: `return 0` on rejected
   input; only a genuine sanitizer trip should abort).
2. Add one line to `fuzz/CMakeLists.txt`: `fl_add_fuzzer(fuzz_<name>)` followed by the
   harness's `target_link_libraries` (link the zero-dep `engine-protocol` seam where the
   parser lives there; compile the owning `.cpp` directly otherwise, as `fuzz_rcon_packet`
   does for `RconServer.cpp`).
3. Mint tiny **synthetic** seeds (never copyrighted assets) into `fuzz/corpus/fuzz_<name>/`.
   Extend `fuzz/mint_seeds.cpp` (reusing the same builders the unit tests use), then run
   `cmake --build --preset fuzz --target fuzz-mint-seeds && ./build/fuzz/fuzz/fuzz-mint-seeds`.
   Regeneration must be byte-stable (`git diff` clean).
4. Optionally add a `fuzz/fuzz_<name>.dict` — it is auto-detected and passed via `-dict=`.

The per-PR CI gate (`fuzz-smoke`) auto-enumerates every `fuzz/fuzz_*.cpp` into a matrix, so
a new harness needs no workflow change.

### Finding policy

When a harness finds a crash:

1. Minimize the reproducer: `./build/fuzz/fuzz/fuzz_<name> -minimize_crash=1 <crash-file>`.
2. Fix the parser.
3. In the **same PR**, commit the minimized input as `fuzz/corpus/fuzz_<name>/regress-*.bin`
   **and** add a Catch2 regression test that feeds those bytes to the parser and asserts the
   fixed behavior. The seed prevents libFuzzer regressions; the unit test documents the bug
   and runs in the fast `debug`/`asan` suites.

### Weekly deep run

`.github/workflows/fuzz-deep.yml` runs **30 minutes per harness** every Tuesday (offset from the
Monday CodeQL sweep) and on `workflow_dispatch`, reaching states the 60 s per-PR smoke never will.
It auto-enumerates `fuzz/fuzz_*.cpp` into a matrix (one job per harness), so a new harness is picked
up with no workflow edit. A finding fails the run red, uploads the minimized-able reproducer as a
per-harness `fuzz-deep-findings-*` artifact, and auto-files (or extends) an open `fuzzing`-labeled
issue titled `fuzz: scheduled deep-run findings` that links the run and restates the finding policy
above. To reproduce a deep-run finding locally, download the artifact and run the harness on it:
`./build/fuzz/fuzz/fuzz_<name> <reproducer>`.

---

## Git setup

### DCO sign-off

This project requires a `Signed-off-by` line on every commit (Developer Certificate of Origin). Always use `git commit -s` or install the commit-msg hook to have it appended automatically:

```bash
cp scripts/hooks/commit-msg .git/hooks/
chmod +x .git/hooks/commit-msg
```

The hook appends `Signed-off-by: Your Name <your@email>` if the line is not already present, so you never accidentally trigger a DCO failure.

### clang-format (pre-commit)

CI enforces clang-format on every changed C/C++ file. Install the pre-commit hook to auto-format staged files before each commit so the check never fails in CI:

```bash
cp scripts/hooks/pre-commit .git/hooks/
chmod +x .git/hooks/pre-commit
```

To install both hooks at once:

```bash
cp scripts/hooks/commit-msg scripts/hooks/pre-commit .git/hooks/
chmod +x .git/hooks/commit-msg .git/hooks/pre-commit
```

---

## IDE setup

### VS Code

Open the repo root. VS Code auto-detects `CMakePresets.json` and populates the configure dropdown via the CMake Tools extension. Recommended extensions and workspace settings are committed in `.vscode/extensions.json` and `.vscode/settings.json` — VS Code prompts to install them on first open.

Pre-defined tasks are committed in `.vscode/tasks.json` and available via **Terminal → Run Task**:

| Task | Purpose |
|---|---|
| Build (Debug) / Build (Release) | Configure + build the selected preset |
| Test (Debug) | `ctest --preset debug --output-on-failure` |
| Coverage (engine/ branch summary) | Build coverage preset, run all tests, print `engine/` branch % vs 80% gate |
| CI: clang-format check | Dry-run clang-format-22 on files changed vs `origin/main` |
| CI: REUSE lint | Check SPDX headers on all source files |
| CI: Smoke tests | Run `--version` on every built binary + net_check ENet loopback |
| CI: pytest (gen_terrain_chunks) | Python unit tests for the terrain chunk pipeline |
| CI: pytest (gen_unifont_header) | Python unit tests for the Unifont header generator |
| CI: Locale lint | `locale-extract --dry-run` to catch untranslated strings |
| CI: Build (ASAN) / CI: Test (ASAN) | Build and test with AddressSanitizer + UBSan (requires `clang`) |
| **CI: All (Linux)** | Run every check above in sequence — use before opening a PR |

### CLion

CLion reads `CMakePresets.json` natively since 2022.3. File → Open → select the repo root. CLion loads all configure/build/test presets automatically.

### Visual Studio 2026

File → Open → CMake → select `CMakeLists.txt`. Visual Studio reads `CMakePresets.json` and shows all presets in the configuration dropdown.

---

## Project structure

```
fighters-legacy/
├── engine/             # Engine core: content system, asset manager, IContentPack
├── platform/           # HAL interfaces (*.h) and backends
│   ├── sdl3/           # SDL3 windowing and input backend
│   ├── vulkan/         # Vulkan renderer backend
│   ├── openal/         # OpenAL Soft audio backend
│   └── net/            # INetwork backends: enet6, GameNetworkingSockets, createNetwork() facade
├── game/               # Game binaries
│   └── fighters-legacy/  # fighters-legacy game client (Phase 1 stub)
├── server/             # Dedicated server binary
│   └── fl-server/      # fl-server — authoritative headless game server
├── tools/              # Asset pipeline and dev utilities — compiled by CMake (net_check, tex-compress, blender_gen.py, …)
├── tests/              # Test suite (Catch2 via FetchContent)
├── docs/               # Documentation
└── scripts/            # Repo-admin shell scripts and git hooks (release, tagging, branch maintenance)
```

The `game/` directory holds game binary entry points. The `server/` directory holds the authoritative dedicated server. `tools/` contains asset-pipeline programs and dev utilities that are part of the CMake build; `scripts/` contains maintainer shell scripts (releases, hooks, branch cleanup) that are not part of the build.

---

## Dependencies

| Dependency | Version | Source |
|---|---|---|
| CMake | 3.25+ | System / installer |
| Vulkan SDK | 1.3+ | LunarG |
| MoltenVK | bundled with Vulkan SDK | LunarG (macOS) |
| SDL3 | 3.4.10 | FetchContent (static) or system (shared, optional) |
| OpenAL Soft | 1.24.2 | FetchContent (static) or system (shared, optional) |
| enet6 | v6.1.3 (SirLynix/enet6) | FetchContent |
| Catch2 | 3.15.0 | FetchContent |
| tomlplusplus | 3.4.0 | FetchContent or system |
| GLM | 1.0.3 | FetchContent or system |
| VulkanMemoryAllocator | 3.4.0 | FetchContent (Vulkan builds only) |
| KTX-Software | 4.4.2 | FetchContent, always static (Vulkan builds only) |
| tinygltf | 3.0.0 | FetchContent or system |
| yaml-cpp | 0.9.0 | FetchContent or system |
| Lua | 5.5.0 | FetchContent or system |

FetchContent fallback is used when the system package is absent or below the required version. The CMake configuration prints the source (system vs fetched) for each dependency.

For links to upstream documentation for each dependency, see [`docs/developer/references.md`](references.md).

---

## CI vs local platform

CI runs on `ubuntu-latest`, `windows-latest`, and `macos-latest`. The maintainer's primary dev platform is Fedora — this is intentional. Both are Linux x86-64 with GCC/Clang; using different distros catches platform-specific assumptions (e.g. library paths, default compiler versions) earlier than a perfectly matched environment would.

See `.github/workflows/ci.yml` for the full three-platform matrix.

### Workflow structure

A **`lint`** job (REUSE + `clang-format-22`) gates the `build` matrix via `needs: [lint]`, so a
formatting failure fails fast without burning macOS/Windows minutes.

The build job's **"Verify static linking (Linux)"** step `ldd`s the game binary (rejecting dynamic
`sdl3|openal|ktx`) and `fl-server` (rejecting `sdl3|vulkan|openal` — no client backend). That is the
binary-level backstop for `cmake/layering.cmake`: the guard fails configure, this fails the build.

Ubuntu apt dependencies are centralised in `.github/actions/install-linux-deps/` — a composite
action with boolean inputs `vulkan`, `gcc`, `clang`, `clang_format`, `python_tools`, `lcov`, `gns`.
**A new Python tool dependency goes there**, plus the matching input set to `'true'` in the ci.yml
build job.

| Workflow | Trigger | What it is |
|---|---|---|
| `ci.yml` | every PR + push | The three-platform build/test matrix, the install-set assertion, the Python tool unit tests + GDAL smokes, and the fuzz chain below. |
| `coverage.yml` | PR + push to main | The `engine/` coverage gates and the Codecov upload. Advisory (not a required check); a failing run on `main` opens a self-closing tracking issue. |
| `asan.yml` | PR | ASan + UBSan. |
| `tsan.yml` | PR | ThreadSanitizer, **Ubuntu only** (no MSVC support). Scoped to the data-parallel sim targets (`test_job_system`, `test_world_broadcaster`) which link no SDL3/OpenAL/ENet/Vulkan; `tools/tsan.supp` suppresses third-party threads. |
| `scale-gate.yml` | PR + nightly | See below. |
| `docs-drift.yml` | PR | Runs `tools/docs_drift.py`, which diffs documented surfaces against the code **both ways**. `input-keys` additionally reads the **Key** column of every user-guide key table and resolves it against `applyDefaults()`, so a table must carry a `Binding` column naming the `InputAction` — see [Controls](../user-guide/controls.md). |
| `fuzz-deep.yml` | weekly | 30 min per harness; auto-files a `fuzzing`-labelled findings issue. |

**Scale gate** (#520) is the 128-client perf/soak gate. The Linux `pr-gate` job (the
`release-headless` preset: Release `fl-server` + `bot_swarm`, no Vulkan, no GNS) hard-fails on bandwidth
(≤ 150 KB/s/client), admission, and a `downstream_kbs_per_client` baseline regression on every PR —
tick-Hz is a collapse tripwire and tick-ms p99 is advisory, because shared runners cannot be trusted
for latency. A `windows-smoke` job runs `run_loadtest.ps1` (8 clients) to keep the launcher from
bitrotting. A `reference-gate` job (nightly cron + `workflow_dispatch`) runs the 128-client
`reference`/`soak` profiles, GNS-primary since #773 (both ends on GameNetworkingSockets, built
`FL_ENABLE_GNS=ON`; `reference-enet` is the enet6 regression leg), and applies the strict tick-ms p99
(≤ 16.6) only with `--strict` on an 8-core reference/self-hosted runner. Driven by
`tools/bot_swarm/scale_gate.py` over `scale-gate.json`, against the committed
`scale-gate-baseline.json`.

**Fuzzing** (#94) is the `fuzz-changes → fuzz-run → fuzz-smoke` chain in ci.yml. `fuzz-changes`
path-filters and auto-enumerates `fuzz/fuzz_*.cpp` into a per-harness matrix; `fuzz-run` builds and
runs each for a 60 s smoke; the stable `fuzz-smoke` aggregate is the **required** check, and is green
when no fuzzable file changed. The `fuzz` preset is clang/Linux-only, so the MSVC/GCC matrix is
unaffected.

---

## Your commit subject is the changelog entry

`CHANGELOG.md` is generated from conventional-commit subjects (#1123). **Feature PRs do not touch
it** — a hand-written entry means every pair of open PRs conflicts on the same list, which is what
this replaced. The trade-off is deliberate: an entry is now one line rather than a paragraph of
rationale, and the rationale lives in the commit body and the PR description instead.

So the subject has to carry its own weight. Write it as a statement to a player or an operator:

```
# reads well as a published changelog line
fix(game): degrade to silent audio instead of refusing to start
feat(game): unlimited per-action bindings across multiple input devices
fix(server): report the server's uptime, not the machine's

# does not
fix(game): fix audio bug                     # names no behaviour, and "fix" twice says nothing
refactor(engine): address review feedback    # describes the process, not the change
feat(net): part 2                            # meaningless outside the PR it was written in
```

Because `main` is squash-merged, the **PR title** becomes the commit subject — so the PR title is
what gets published. `cliff.toml` decides which types appear at all: `feat` → **Added**,
`fix` → **Fixed**, `docs`/`refactor`/`perf`/`revert` → **Changed**, and `ci`/`chore`/`build`/`test`/
`style` are skipped entirely. A user-facing change typed `chore:` will not appear in any release.
A `!` marker or a `BREAKING CHANGE:` footer prefixes the entry with **BREAKING** and survives even
the skipped types.

## Release workflow

Releases are tagged with `vMAJOR.MINOR.PATCH`. The `release.yml` workflow triggers on version tags,
builds all three platforms, and creates a GitHub Release with SLSA build provenance.

> **The canonical procedure is [`project-management.md` → *Cutting a release*](project-management.md#cutting-a-release).**
> This section covers only the two scripts. **The release is not finished when the tag is pushed** —
> the archives must be verified and the release notes hand-authored afterwards, and those steps have
> been skipped often enough to warrant saying so twice.

### Step 1 — Create the release PR

From a clean `main`:

```bash
git checkout main && git pull origin main
./scripts/cut-release.sh v0.1.0
```

This creates a `release/v0.1.0` branch, runs `scripts/gen_changelog.py` to write a
`[0.1.0] - <today>` CHANGELOG section from the conventional-commit subjects since the last tag,
bumps the CMake version, commits and pushes.

`git-cliff` is a prerequisite of this step — `sudo dnf install git-cliff` on Fedora,
`brew install git-cliff` on macOS, or `cargo install git-cliff` anywhere. `gen_changelog.py`
refuses with the install lines if it is missing, and refuses a git-cliff older than 2.0.

Before opening the PR, read the generated section as a player would and check its scope against
`git log --oneline <prev-tag>..HEAD`. Then open the printed PR URL, wait for CI, and merge.

### Step 2 — Tag and trigger the release

After the PR merges:

```bash
git checkout main && git pull origin main
./scripts/tag-release.sh v0.1.0
```

This tags `main` and pushes the tag; `release.yml` fires immediately. The script refuses to tag if
the CHANGELOG's section date is not today, so the changelog, the tag and the release body agree.

### Step 3 — Finish the release

The tag is not the end. Once `release.yml` has **completed** (its release step overwrites the body,
so anything applied earlier is lost):

```bash
gh release view v0.1.0 --json assets -q '.assets[] | "\(.name) \(.size)"'   # 3 archives?
gh release edit v0.1.0 --notes-file <file>                                  # hand-authored notes
gh release view v0.1.0 --json body -q '.body' | head -3                     # read back, right tag?
```

The release-notes gate (`.github/workflows/release-notes-gate.yml`) checks the last two
automatically and opens a tracking issue if the body is autogenerated or describes the wrong
version. See [`project-management.md`](project-management.md#cutting-a-release) for what a release
body contains and why each check exists.

---

## fl-server

`fl-server` is the headless dedicated server binary (`server/fl-server/`). It
owns the authoritative sim loop (EntityManager + GameLoop), serialises world
state via the binary game protocol, and runs without a window, renderer, audio,
or input.

### Prerequisites

No system install needed — enet6 is fetched automatically via FetchContent.
All other prerequisites are the same as the base debug build (CMake 3.25+,
a C++20-capable compiler, and the standard project deps from the sections above).

### Build

```bash
cmake --preset debug
cmake --build --preset debug --target fl-server
# Binary: build/debug/server/fl-server/fl-server
```

### Run

```bash
# Defaults: port 4778, 16 peers
./build/debug/server/fl-server/fl-server

# Override port and peer count via positional args
./build/debug/server/fl-server/fl-server 4778 4

# Flags
./build/debug/server/fl-server/fl-server --help
./build/debug/server/fl-server/fl-server --version
```

### Content and terrain

`fl-server` scans the `mods/` subdirectory of its working directory for content packs on
startup. Place a content pack directory (containing a valid `manifest.toml`) there to load
real terrain data. With no content packs present, the server uses the built-in procedural
terrain (FBM heightmap, ~550 m base elevation) and all height queries return procedurally
generated values.

### Configuration

`fl-server` resolves settings in three tiers: `server.toml` (lowest priority) →
CLI positional args → environment variables (highest priority). The config file is
written with commented defaults on first run if absent; an absent or unreadable file
is not fatal.

For the full configuration reference — all TOML sections, keys, types, defaults, valid
values, CLI flags, and env var mappings — see
[docs/server-ops/server-config.md](../server-ops/server-config.md).

Quick reference for container deployments:

| Variable | Default | Purpose |
|---|---|---|
| `FL_CONFIG` | `./server.toml` | Path to config file |
| `FL_PORT` | `4778` | UDP bind port |
| `FL_BIND_ADDRESS` | `0.0.0.0` | Bind address (use `127.0.0.1` for localhost-only) |
| `FL_MAX_PEERS` | `32` | Maximum simultaneous connected peers |
| `FL_NAME` | `"Unnamed Server"` | Server name shown in the lobby |
| `FL_LOBBY_REGISTER` | `"false"` | Set `"true"` to advertise to an fl-lobby named by `FL_LOBBY_URL` |
| `FL_AI_DIFFICULTY_FLOOR` | `"recruit"` | Minimum AI difficulty (Phase 2) |

Additional lobby env vars (`FL_LOBBY_URL`, `FL_LOBBY_VISIBILITY`) are documented in
the full reference.

### Kubernetes / containers

See [docs/server-ops/server-config.md — Kubernetes / container deployment](../server-ops/server-config.md#kubernetes--container-deployment).

### Stop

`Ctrl-C` or `SIGTERM` triggers a graceful disconnect and exits 0.

---

## net_check

`net_check` is a headless developer utility (`tools/net_check/`) for
smoke-testing the enet6 transport layer. It connects to a running `fl-server`,
sends periodic ping packets, then disconnects cleanly. It is not a game client.

### Build

```bash
cmake --preset debug
cmake --build --preset debug --target net_check
# Binary: build/debug/tools/net_check
```

### Usage

```bash
net_check [host] [port] [--count N] [--interval MS]
```

| Argument | Default | Purpose |
|---|---|---|
| `host` | `127.0.0.1` | Server hostname or IP |
| `port` | `4778` | Server port |
| `--count N` / `-n N` | unlimited | Send N packets then disconnect |
| `--interval MS` | `1000` | Milliseconds between pings |

Environment variables `FL_HOST` and `FL_PORT` set the defaults when the
positional args are omitted.

### End-to-end smoke test

```bash
# Terminal 1 — start server
./build/debug/server/fl-server/fl-server 4778 4

# Terminal 2 — send 5 pings at 500 ms intervals, then exit
./build/debug/tools/net_check 127.0.0.1 4778 --count 5 --interval 500
```

Expected server output: `peer 0 connected` → `peer 0 disconnected`.
Expected client output: `connecting` → `connected` → pings → `disconnecting`.

---

## bot_swarm

`bot_swarm` (`tools/bot_swarm/`) is the headless multi-client **load generator** — the
multi-client companion to `net_check`. It connects N synthetic game clients to a running
`fl-server`, sustains `MsgClientInput` with a pluggable flight pattern, and reports the
client-observable scale metrics (observed server tick-Hz, downstream KB/s per client, RTT).
See [docs/developer/load-testing.md](load-testing.md) for the full reference and the ceiling-characterisation
runbook.

### Build

```bash
cmake --build --preset debug --target fl-server bot_swarm
# Binary: build/debug/tools/bot_swarm
```

### Usage

```bash
# Turnkey: the runner launches fl-server with a load-test config, then drives the swarm.
tools/bot_swarm/run_loadtest.sh build/debug 128 30 weave
# -> tools/bot_swarm/results/loadtest_128c_weave_<ts>.json

# Or against an already-running server:
bot_swarm 127.0.0.1 4778 --clients 128 --duration 30 --pattern weave --json out.json
```

> The connect-rate-limit/per-IP caps come only from `server.toml` — the default
> `connect_rate_limit_count = 5` rejects a rapid 128-client ramp. `run_loadtest.sh` writes the
> required load-test config; see [docs/developer/load-testing.md](load-testing.md).

### Reference load-test environment (optional)

For *comparable* numbers, run the sweep inside the fixed **8-core / 16 GB** reference profile in
[`tools/bot_swarm/reference-env/`](https://github.com/fighters-legacy/fighters-legacy/blob/main/tools/bot_swarm/reference-env/README.md) rather than
ad-hoc on your dev box (and always build **Release** — Debug numbers are pessimistic). Two
paths, both optional and not needed for normal development:

- **Container** (fast): `podman` (preferred on Fedora) or Docker. Then:

  ```bash
  tools/bot_swarm/reference-env/run-container.sh        # builds Release inside, sweeps under the cap
  ```

  **Rootless cpuset delegation (Linux, one-time).** Pinning to 8 cores (`--cpuset-cpus`) needs
  the `cpuset` cgroup controller delegated to your user slice — by default it isn't, and the
  wrapper falls back to a `--cpus` quota (the guest then still sees all host cores). To pin for
  real:

  ```bash
  sudo mkdir -p /etc/systemd/system/user@.service.d
  printf '[Service]\nDelegate=cpu cpuset io memory pids\n' \
      | sudo tee /etc/systemd/system/user@.service.d/delegate.conf
  sudo systemctl daemon-reload      # then log out / back in
  # verify: podman run --rm --cpuset-cpus 0-7 <image> nproc  -> 8
  ```

  On a hybrid CPU (Intel P/E cores) set `CPUSET` to 8 performance-core threads (`lscpu -e`).

- **VM** (most faithful — own kernel, dedicated vCPUs): `vagrant` + `libvirt`.

  ```bash
  sudo dnf install -y vagrant && sudo systemctl enable --now libvirtd
  sudo usermod -aG libvirt "$USER"            # then re-login
  cd tools/bot_swarm/reference-env && vagrant up
  vagrant ssh -c 'sudo bash /src/tools/bot_swarm/reference-env/run-benchmark.sh'
  ```

Windows (Docker Desktop + `.wslconfig`) and macOS (`podman machine`/Docker Desktop sizing,
Apple-Silicon arm64 caveat) are covered in the
[reference-env README](https://github.com/fighters-legacy/fighters-legacy/blob/main/tools/bot_swarm/reference-env/README.md).

There is a second VM in the repo and the two are not interchangeable: this one is a **pinned
measurement** environment and is where performance numbers come from, while
[`tools/windows-env/`](https://github.com/fighters-legacy/fighters-legacy/blob/main/tools/windows-env/README.md)
is a **correctness** environment for running the MSVC toolchain locally. Never benchmark in the
Windows VM — it is sized off the host rather than pinned, and renders through software Vulkan.

---

## locale-extract

`locale-extract` is a developer tool that keeps source key references and
`locale/en/*.toml` files in sync. It is also the CI gate for locale drift.

### Build

```bash
cmake --preset debug
cmake --build --preset debug --target locale-extract
# Binary: build/debug/tools/locale-extract
```

### Usage

```bash
locale-extract [--src <dir>] [--locale <dir>] [--gen-keys <output>] [--dry-run]
```

| Flag | Default | Purpose |
|---|---|---|
| `--src <dir>` | `engine/` | Directory tree to scan for `.get()`/`.format()`/`.getPlural()` calls |
| `--locale <dir>` | `locale` | Root locale directory containing `en/` |
| `--dry-run` | off | Report new/orphaned keys without modifying any files |
| `--gen-keys <output>` | — | Write `generated/i18n/LocaleKeys.h` with `constexpr` key constants |

### Lint mode (default)

Scans `--src` for key references, compares them against `locale/en/*.toml`,
and prints a `+`/`-` diff of new and orphaned keys. Exits 1 if any drift is
found; exits 0 if all keys are in sync.

```bash
# Check for drift without modifying files
./build/debug/tools/locale-extract --src engine --locale locale --dry-run

# Inject missing keys into locale/en/*.toml (preserves comments)
./build/debug/tools/locale-extract --src engine --locale locale
```

New keys are injected with `= ""` so translators can fill them in. Existing
content — including `# translator context comments` — is never rewritten.

This lint also runs as the `locale_lint` CTest and as the
`.github/workflows/locale-lint.yml` CI job on every push and PR.

### Regenerating the embedded HUD font

`platform/vulkan/UnifontBitmap.{h,cpp}` are generated from GNU Unifont and committed. Re-run only when upgrading the Unifont version:

```bash
python3 tools/gen_unifont_header.py --output-dir platform/vulkan
# Downloads unifont-16.0.02.hex.gz from unifoundry.com, writes UnifontBitmap.{h,cpp}.
# Commit both generated files. Re-run tools/gen_unifont_header.py --help for options.
```

### Key constants (optional)

```bash
# Generate LocaleKeys.h (compiler catches key typos)
cmake --build --preset debug --target locale-keys
# Header at: build/debug/generated/i18n/LocaleKeys.h
```

```cpp
// Usage in engine code:
#include "generated/i18n/LocaleKeys.h"
loc.get(keys::engine::content::pack_init_failed);
```

See [docs/modding/localization.md](../modding/localization.md) for the full
translator workflow and mod locale directory layout.

---

## fighters-legacy

`fighters-legacy` is the game client binary. In Phase 1 it is a stub that exercises the crash
reporting, logging, and mod loading systems without a playable game loop. The full game loop,
HUD, and menus land in Phase 2.

### Build

```bash
cmake --preset debug
cmake --build --preset debug --target fighters-legacy
# Binary: build/debug/game/fighters-legacy/fighters-legacy
```

### Run

```bash
# Default startup (Info log level)
./build/debug/game/fighters-legacy/fighters-legacy

# Override log level for this session only (does not persist to user.toml)
./build/debug/game/fighters-legacy/fighters-legacy --log-level debug

# Print version and exit
./build/debug/game/fighters-legacy/fighters-legacy --version
```

### User data directory

| Platform | Path |
|----------|------|
| Linux    | `~/.local/share/mkzsystems/fighters-legacy/` |
| macOS    | `~/Library/Application Support/mkzsystems/fighters-legacy/` |
| Windows  | `%APPDATA%\mkzsystems\fighters-legacy\` |

Session logs are written to `<userdata>/logs/engine_<date>.log` (10 retained).
Crash dumps are written to `<userdata>/logs/crash_<timestamp>.log` (5 retained).

---

## Roadmap status

To report phase completion against target dates:

```bash
./scripts/roadmap-status.sh
```

Queries the repository's [GitHub milestones](https://github.com/fighters-legacy/fighters-legacy/milestones) via `gh` and prints a per-phase progress table showing % done (closed/total issues), % of time elapsed, and an on-track/at-risk/behind/overdue signal. Milestones carry only a due date, so each phase's elapsed window is derived sequentially (a phase starts when the previous one is due). Requires `gh` (authenticated), `jq`, and GNU `date`.

---

## Loopback latency analysis

Tools for re-running the ENet loopback latency analysis across platforms
([docs/developer/decisions/loopback-latency-analysis.md](decisions/loopback-latency-analysis.md)). Not required
to build or run the game.

```
Fedora/RHEL:   sudo dnf install sockperf
Ubuntu/Debian: sudo apt install sockperf
macOS:         brew install iperf3
Windows:       (no extra tools required)
```

---

## Sandbox reference

Key bindings, camera modes, flight controls, and debug console commands are documented in
[docs/user-guide/controls.md](../user-guide/controls.md).
