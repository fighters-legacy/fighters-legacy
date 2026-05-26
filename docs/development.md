# Development Guide

## Prerequisites

### Linux — Fedora (primary maintainer platform)

```bash
sudo dnf install cmake ninja-build gcc g++ clang clang-tools-extra \
  vulkan-devel SDL3-devel openal-soft-devel lcov \
  libasan libubsan glslang vulkan-validation-layers
```

`libasan` and `libubsan` are required for the `asan` build preset (`-fsanitize=address,undefined`). They are separate packages from `gcc` on Fedora.

`glslang` provides `glslangValidator`, the GLSL-to-SPIR-V compiler used to build the Vulkan renderer shaders at CMake configure time.

`vulkan-validation-layers` provides `VK_LAYER_KHRONOS_validation`, enabled automatically in debug builds via `FL_VK_VALIDATION`. Without it the renderer still works but validation errors go unreported.

For Bluetooth gamepad support (Xbox controllers), see [docs/linux-gamepad.md](linux-gamepad.md).

> **Note (OpenAL):** Some Fedora installs ship `/etc/openal/alsoft.conf` with `drivers = null`, which silently discards all audio. If `audio_check` reports success but you hear nothing, override it: `printf '[general]\ndrivers = pipewire\n' > ~/.config/alsoft.conf`

### Linux — Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build gcc g++ clang clang-format \
  libvulkan-dev libsdl3-dev libopenal-dev lcov glslang-tools \
  vulkan-validationlayers
```

`glslang-tools` provides `glslangValidator` for Vulkan shader compilation. `vulkan-validationlayers` provides `VK_LAYER_KHRONOS_validation` for debug builds.

> **Note:** SDL3 and OpenAL Soft may need building from source on older distros if the packaged versions are below the required minimum. Check `cmake/dependencies.cmake` (added in Phase 1) for version requirements.

### Windows (MSVC 2022)

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload
2. Install the [Vulkan SDK](https://vulkan.lunarg.com/) (1.3 or later) — includes `glslangValidator`, `VK_LAYER_KHRONOS_validation`, and MoltenVK support headers
3. Optional: install Ninja via `winget install Ninja-build.Ninja` (faster incremental builds)

### macOS (Apple Silicon, 13+)

```bash
xcode-select --install
brew install cmake ninja vulkan-headers molten-vk vulkan-loader glslang
```

`molten-vk` provides the Vulkan-over-Metal ICD; `vulkan-loader` provides `libvulkan.dylib`; `glslang` provides `glslangValidator`. Validation layers are not available via Homebrew — install the [LunarG Vulkan SDK for macOS](https://vulkan.lunarg.com/) to get `VK_LAYER_KHRONOS_validation`.

> **Note:** CI uses the Homebrew path (no validation layers). For local dev, the LunarG SDK is recommended so validation errors are caught before CI.

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

### CLion

CLion reads `CMakePresets.json` natively since 2022.3. File → Open → select the repo root. CLion loads all configure/build/test presets automatically.

### Visual Studio 2022

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
├── tools/              # Developer utilities (fl-server, fl-client, hello_triangle, …)
├── tests/              # Test suite (Catch2 via FetchContent)
├── docs/               # Documentation
└── scripts/            # Developer scripts and git hooks
```

The `game/` directory holds game binary entry points. Developer tools and headless infrastructure binaries live in `tools/`.

---

## Dependencies

| Dependency | Version | Source |
|---|---|---|
| CMake | 3.25+ | System / installer |
| Vulkan SDK | 1.3+ | LunarG |
| MoltenVK | bundled with Vulkan SDK | LunarG (macOS) |
| SDL3 | latest | FetchContent or system |
| OpenAL Soft | 1.24+ | FetchContent or system |
| ENet | 2.x | FetchContent |
| Catch2 | 3.x | FetchContent |
| tomlplusplus | 3.4+ | FetchContent or system |

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

`fl-server` is the headless dedicated server binary. It implements the ENet
UDP transport backend and runs without a window, renderer, audio, or input.

### Prerequisites

No system install needed — ENet is fetched automatically via FetchContent.
All other prerequisites are the same as the base debug build (CMake 3.25+,
a C++20-capable compiler, and the standard project deps from the sections above).

### Build

```bash
cmake --preset debug
cmake --build --preset debug --target fl-server
# Binary: build/debug/tools/fl-server
```

### Run

```bash
# Defaults: port 4778, 16 peers
./build/debug/tools/fl-server

# Override port and peer count via positional args
./build/debug/tools/fl-server 4778 4

# Flags
./build/debug/tools/fl-server --help
./build/debug/tools/fl-server --version
```

### Configuration

`fl-server` resolves settings in three tiers (later tiers override earlier ones):

1. `server.toml` in the working directory (or path in `FL_CONFIG`). Written
   with commented defaults on first run if absent. Absent or unreadable config
   is not fatal — the server logs a warning and uses defaults.
2. CLI positional args: `fl-server [port] [maxPeers]`
3. Environment variables (highest precedence — recommended for containers):

| Variable | Default | Purpose |
|---|---|---|
| `FL_PORT` | `4778` | UDP bind port |
| `FL_MAX_PEERS` | `16` | Maximum simultaneous connected peers |
| `FL_CONFIG` | `./server.toml` | Path to config file |

### Kubernetes / containers

- Pass all config via environment variables; no volume mount required.
- Optionally mount a pre-baked `server.toml` via ConfigMap — the first-run
  write is skipped when the file already exists.
- All output goes to stdout; compatible with Fluentd, Loki, and similar
  log aggregators.
- Responds to `SIGTERM` (sent by Kubernetes on pod termination) with a 100 ms
  graceful peer disconnect before exit — well within the default
  `terminationGracePeriodSeconds` of 30 s.

### Stop

`Ctrl-C` or `SIGTERM` triggers a graceful disconnect and exits 0.

---

## fl-client

`fl-client` is a headless developer test tool for smoke-testing the full
client/server lifecycle locally. It connects to a running `fl-server`, sends
periodic ping packets, then disconnects cleanly.

### Build

```bash
cmake --preset debug
cmake --build --preset debug --target fl-client
# Binary: build/debug/tools/fl-client
```

### Usage

```bash
fl-client [host] [port] [--count N] [--interval MS]
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
./build/debug/tools/fl-server 4778 4

# Terminal 2 — send 5 pings at 500 ms intervals, then exit
./build/debug/tools/fl-client 127.0.0.1 4778 --count 5 --interval 500
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
| Linux    | `~/.local/share/jomkz/fighters-legacy/` |
| macOS    | `~/Library/Application Support/jomkz/fighters-legacy/` |
| Windows  | `%APPDATA%\jomkz\fighters-legacy\` |

Session logs are written to `<userdata>/logs/engine_<date>.log` (10 retained).
Crash dumps are written to `<userdata>/logs/crash_<timestamp>.log` (5 retained).

---

## Roadmap status

To report phase completion against target dates:

```bash
./scripts/roadmap-status.sh
```

Queries the [GitHub Project](https://github.com/users/jomkz/projects/2) via `gh` and prints a per-phase progress table showing % done, % of time elapsed, and an on-track/at-risk/behind/overdue signal. Requires `gh` (authenticated) and GNU `date`.
