# Development Guide

## Prerequisites

### Linux — Fedora (primary maintainer platform)

```bash
sudo dnf install cmake ninja-build gcc g++ clang clang-tools-extra \
  vulkan-devel SDL3-devel openal-soft-devel lcov \
  libasan libubsan
```

`libasan` and `libubsan` are required for the `asan` build preset (`-fsanitize=address,undefined`). They are separate packages from `gcc` on Fedora.

### Linux — Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build gcc g++ clang clang-format \
  libvulkan-dev libsdl3-dev libopenal-dev lcov
```

> **Note:** SDL3 and OpenAL Soft may need building from source on older distros if the packaged versions are below the required minimum. Check `cmake/dependencies.cmake` (added in Phase 1) for version requirements.

### Windows (MSVC 2022)

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload
2. Install the [Vulkan SDK](https://vulkan.lunarg.com/) (1.3 or later)
3. Optional: install Ninja via `winget install Ninja-build.Ninja` (faster incremental builds)

### macOS (Apple Silicon, 13+)

```bash
xcode-select --install
brew install cmake ninja
```

Install the [Vulkan SDK for macOS](https://vulkan.lunarg.com/) from LunarG (includes MoltenVK).

### Optional tools

**REUSE** — checks SPDX license compliance. CI enforces this automatically via `fsfe/reuse-action`; install locally for fast feedback before pushing:

```bash
pip install reuse
```

Run with `reuse lint` from the repo root.

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

## Code coverage (optional — CI handles this automatically)

```bash
cmake --preset coverage
cmake --build --preset coverage
ctest --preset coverage --output-on-failure
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/tests/*' '*/vendor/*' \
     --output-file coverage.info
```

Codecov posts a PR coverage delta comment automatically. You do not need to run coverage locally unless you are investigating a specific threshold failure.

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
├── tools/              # Developer tools (added Phase 1)
├── tests/              # Test suite (Catch2 via FetchContent)
├── docs/               # Documentation
└── scripts/            # Developer scripts and git hooks
```

Structure is populated as Phase 1 Workstream A engine code lands. The stubs above reflect the planned layout.

---

## Dependencies

| Dependency | Version | Source |
|---|---|---|
| CMake | 3.25+ | System / installer |
| Vulkan SDK | 1.3+ | LunarG |
| MoltenVK | bundled with Vulkan SDK | LunarG (macOS) |
| SDL3 | latest | FetchContent or system |
| OpenAL Soft | 1.23+ | FetchContent or system |
| ENet | 2.x | FetchContent |
| Catch2 | 3.x | FetchContent |
| tomlplusplus | 3.4+ | FetchContent or system |

FetchContent fallback is used when the system package is absent or below the required version. The CMake configuration prints the source (system vs fetched) for each dependency.

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

## Roadmap status

To report phase completion against target dates:

```bash
./scripts/roadmap-status.sh
```

Queries the [GitHub Project](https://github.com/users/jomkz/projects/2) via `gh` and prints a per-phase progress table showing % done, % of time elapsed, and an on-track/at-risk/behind/overdue signal. Requires `gh` (authenticated) and GNU `date`.
