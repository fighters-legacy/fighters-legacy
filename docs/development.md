# Development Guide

## Prerequisites

### Linux — Fedora (primary maintainer platform)

```bash
sudo dnf install cmake ninja-build gcc g++ clang clang-tools-extra \
  vulkan-devel SDL3-devel openal-soft-devel lcov \
  libasan libubsan glslang vulkan-validation-layers lua-devel
```

`libasan` and `libubsan` are required for the `asan` build preset (`-fsanitize=address,undefined`). They are separate packages from `gcc` on Fedora.

`glslang` provides `glslangValidator`, the GLSL-to-SPIR-V compiler used to build the Vulkan renderer shaders at CMake configure time.

`vulkan-validation-layers` provides `VK_LAYER_KHRONOS_validation`, enabled automatically in debug builds via `FL_VK_VALIDATION`. Without it the renderer still works but validation errors go unreported.

For Bluetooth gamepad support (Xbox controllers), see [docs/linux-gamepad.md](linux-gamepad.md).

> **Note:** `SDL3-devel` and `openal-soft-devel` are optional. CMake automatically fetches and statically compiles both when they are absent — this is what CI and release builds do. Installing them speeds up local dev builds but means your dev binary will dynamically link those libraries, unlike the self-contained CI and release artifacts.

> **Note (OpenAL):** Some Fedora installs ship `/etc/openal/alsoft.conf` with `drivers = null`, which silently discards all audio. If `audio_check` reports success but you hear nothing, override it: `printf '[general]\ndrivers = pipewire\n' > ~/.config/alsoft.conf`

### Linux — Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build gcc g++ clang clang-format \
  libvulkan-dev libsdl3-dev libopenal-dev lcov glslang-tools \
  vulkan-validationlayers
```

`glslang-tools` provides `glslangValidator` for Vulkan shader compilation. `vulkan-validationlayers` provides `VK_LAYER_KHRONOS_validation` for debug builds.

> **Note:** `libsdl3-dev` and `libopenal-dev` are optional. CMake automatically fetches and statically compiles both when they are absent — this is what CI and release builds do. Installing them speeds up local dev builds but means your dev binary will dynamically link those libraries, unlike the self-contained CI and release artifacts.
>
> **Note (Lua):** `liblua5.5-dev` is not yet available in Ubuntu apt. Lua 5.5 is always built from source via FetchContent on Linux — no extra install needed.

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

Copyright is declared centrally in `REUSE.toml` rather than in each file. All `.h` and `.cpp` files are covered by a glob annotation there — new source files do not need an in-file `SPDX-FileCopyrightText` line. The `// SPDX-License-Identifier: GPL-3.0-or-later` line in each source file is still required (see `CLAUDE.md`).

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
| `debug-msvc` | Windows | Development (Werror ON) |
| `release-msvc` | Windows | Packaging |
| `coverage` | Linux / macOS | Coverage reporting (Werror OFF) |
| `asan` | Linux / macOS | AddressSanitizer + UBSan |

CI uses `debug` (Linux/macOS) and `debug-msvc` (Windows). The `coverage` and `asan` presets have their own dedicated CI jobs.

### Local preset overrides

Create a `CMakeUserPresets.json` in the repo root to override preset defaults locally (e.g. a different binary directory or a custom toolchain path). This file is gitignored.

---

## Testing

For running the test suite see [Building → Running tests](#running-tests).

### Code coverage

CI enforces the following gates on every push and pull request:

| Scope | Metric | Threshold |
|---|---|---|
| `engine/` | Branch coverage | ≥ 80% (CI enforced) |
| `engine/` | Line coverage | ≥ 70% (CI enforced) |

`platform/` backends, `tools/`, and `game/` are excluded from coverage reporting. Branch
coverage catches untested conditional paths and is the primary gate; line coverage is a
secondary floor. `platform/` is excluded because platform divergence makes unit testing
brittle. `game/` is excluded in Phase 1 — the binary is a stub with no game loop; its
coverage gate will be added when Phase 2 game logic lands.

Gates use `--exclude-throw-branches` (gcovr 8.x): GCC instruments every non-`noexcept`
call site with an "exception throw" branch that is never taken in normal unit tests.
Excluding these focuses the gate on meaningful decision branches (if/else, switch).

An HTML coverage report is uploaded as a CI artifact on every run (retained 30 days).
Access it from **Actions** → select a run → **Artifacts → coverage-report**.

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

# Check gates (requires: pip install gcovr)
gcovr --filter 'engine/' --exclude-throw-branches --fail-under-branch 80 --print-summary
gcovr --filter 'engine/' --exclude-throw-branches --fail-under-line 70 --print-summary

# Generate HTML report
genhtml coverage.info --output-directory coverage-report --branch-coverage
# Open coverage-report/index.html
```

Codecov also posts a PR coverage delta comment automatically.

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
│   └── net/            # ENet networking backend
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

For links to upstream documentation for each dependency, see [`docs/references.md`](references.md).

---

## CI vs local platform

CI runs on `ubuntu-latest`, `windows-latest`, and `macos-latest`. The maintainer's primary dev platform is Fedora — this is intentional. Both are Linux x86-64 with GCC/Clang; using different distros catches platform-specific assumptions (e.g. library paths, default compiler versions) earlier than a perfectly matched environment would.

See `.github/workflows/ci.yml` for the full three-platform matrix.

---

## Release workflow

Releases are tagged with `vMAJOR.MINOR.PATCH`. The `release.yml` workflow triggers on version tags, builds all three platforms, generates release notes via git-cliff, and creates a GitHub Release with SLSA build provenance.

**Requires `git-cliff`:** `cargo install git-cliff` | `dnf install git-cliff` | `brew install git-cliff`

### Step 1 — Create the release PR

From a clean `main`:

```bash
git checkout main && git pull origin main
./scripts/cut-release.sh v0.1.0
```

This creates a `release/v0.1.0` branch, generates `CHANGELOG.md`, commits, and pushes.
Open the printed PR URL, wait for CI, and merge.

### Step 2 — Tag and trigger the release

After the PR merges:

```bash
git checkout main && git pull origin main
./scripts/tag-release.sh v0.1.0
```

This tags `main` and pushes the tag. The `release.yml` workflow fires immediately.

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
[docs/fl-server-config.md](fl-server-config.md).

Quick reference for container deployments:

| Variable | Default | Purpose |
|---|---|---|
| `FL_CONFIG` | `./server.toml` | Path to config file |
| `FL_PORT` | `4778` | UDP bind port |
| `FL_BIND_ADDRESS` | `0.0.0.0` | Bind address (use `127.0.0.1` for localhost-only) |
| `FL_MAX_PEERS` | `32` | Maximum simultaneous connected peers |
| `FL_NAME` | `"Unnamed Server"` | Server name shown in the lobby |
| `FL_PERSISTENT` | `"false"` | Set `"true"` to enable persistent world mode (Phase 2) |
| `FL_LOBBY_REGISTER` | `"false"` | Set `"true"` to advertise to fl-lobby (Phase 2) |
| `FL_AI_DIFFICULTY_FLOOR` | `"recruit"` | Minimum AI difficulty (Phase 2) |

Additional Phase 2 env vars (`FL_LOBBY_URL`, `FL_LOBBY_VISIBILITY`) are documented in
the full reference.

### Kubernetes / containers

See [docs/fl-server-config.md — Kubernetes / container deployment](fl-server-config.md#kubernetes--container-deployment).

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

See [docs/modding/localization.md](modding/localization.md) for the full
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
([docs/loopback-latency-analysis.md](loopback-latency-analysis.md)). Not required
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
[docs/sandbox.md](sandbox.md).
